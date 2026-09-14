[CmdletBinding()]
param(
    [string]$AssetRoot = (Join-Path $PSScriptRoot "..\agent\assets"),

    [ValidateRange(16, 256)]
    [int]$MaxColors = 256,

    [switch]$DisableDithering
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# System.Drawing is used deliberately here: this helper is run by the Windows
# VitaDevDeploy build host and does not add another image-tool dependency.
Add-Type -AssemblyName System.Drawing

if (-not ([System.Management.Automation.PSTypeName]"VitaDevDeploy.LiveArea.AssetPipeline").Type) {
    $quantizerSource = @'
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.IO;
using System.Runtime.InteropServices;

namespace VitaDevDeploy.LiveArea
{
    public sealed class PreparedAsset
    {
        public int Width { get; internal set; }
        public int Height { get; internal set; }
        public int UsedColors { get; internal set; }
        public bool HasTransparency { get; internal set; }
        public double MeanAbsoluteRgbError { get; internal set; }
        public int MaximumRgbError { get; internal set; }
    }

    internal sealed class HistogramBin
    {
        internal int Key;
        internal long Count;
        internal long SumR;
        internal long SumG;
        internal long SumB;

        internal int R { get { return (int)((SumR + Count / 2) / Count); } }
        internal int G { get { return (int)((SumG + Count / 2) / Count); } }
        internal int B { get { return (int)((SumB + Count / 2) / Count); } }
    }

    internal sealed class ColorBox
    {
        internal List<HistogramBin> Bins;
        internal long Population;
        internal int MinR;
        internal int MaxR;
        internal int MinG;
        internal int MaxG;
        internal int MinB;
        internal int MaxB;

        internal ColorBox(List<HistogramBin> bins)
        {
            Bins = bins;
            Recalculate();
        }

        internal void Recalculate()
        {
            Population = 0;
            MinR = MinG = MinB = 255;
            MaxR = MaxG = MaxB = 0;
            for (int i = 0; i < Bins.Count; ++i)
            {
                HistogramBin bin = Bins[i];
                int r = bin.R;
                int g = bin.G;
                int b = bin.B;
                Population += bin.Count;
                if (r < MinR) MinR = r;
                if (r > MaxR) MaxR = r;
                if (g < MinG) MinG = g;
                if (g > MaxG) MaxG = g;
                if (b < MinB) MinB = b;
                if (b > MaxB) MaxB = b;
            }
        }

        internal bool CanSplit { get { return Bins.Count > 1; } }

        internal long SplitScore
        {
            get
            {
                long rr = MaxR - MinR;
                long gg = MaxG - MinG;
                long bb = MaxB - MinB;
                return Population * (2L * rr * rr + 4L * gg * gg + bb * bb);
            }
        }
    }

    internal sealed class BinChannelComparer : IComparer<HistogramBin>
    {
        private readonly int _channel;

        internal BinChannelComparer(int channel)
        {
            _channel = channel;
        }

        public int Compare(HistogramBin left, HistogramBin right)
        {
            int a;
            int b;
            if (_channel == 0)
            {
                a = left.R;
                b = right.R;
            }
            else if (_channel == 1)
            {
                a = left.G;
                b = right.G;
            }
            else
            {
                a = left.B;
                b = right.B;
            }

            int order = a.CompareTo(b);
            if (order != 0) return order;
            return left.Key.CompareTo(right.Key);
        }
    }

    public static class AssetPipeline
    {
        private const int TransparentAlphaThreshold = 128;

        public static PreparedAsset Prepare(
            string sourcePath,
            string outputPath,
            int width,
            int height,
            int maxColors,
            bool flatten,
            int matteR,
            int matteG,
            int matteB,
            bool dither)
        {
            if (width <= 0 || height <= 0)
                throw new ArgumentOutOfRangeException("Target dimensions must be positive.");
            if (maxColors < 16 || maxColors > 256)
                throw new ArgumentOutOfRangeException("maxColors");

            int[] pixels;
            using (Bitmap resized = ResizeCover(sourcePath, width, height))
                pixels = ReadPixels(resized, flatten, matteR, matteG, matteB);

            bool hasTransparency = false;
            if (!flatten)
            {
                for (int i = 0; i < pixels.Length; ++i)
                {
                    if (((pixels[i] >> 24) & 255) < TransparentAlphaThreshold)
                    {
                        hasTransparency = true;
                        break;
                    }
                }
            }

            int opaqueColorLimit = maxColors - (hasTransparency ? 1 : 0);
            Dictionary<int, HistogramBin> histogram = BuildHistogram(pixels, hasTransparency);
            if (histogram.Count == 0)
                throw new InvalidDataException("The image does not contain any opaque pixels.");

            List<Color> colors = BuildPalette(histogram, opaqueColorLimit);
            double meanError;
            int maximumError;
            byte[] indices = MapPixels(
                pixels,
                width,
                height,
                colors,
                hasTransparency,
                dither,
                out meanError,
                out maximumError);

            string outputDirectory = Path.GetDirectoryName(outputPath);
            if (!String.IsNullOrEmpty(outputDirectory))
                Directory.CreateDirectory(outputDirectory);

            string temporaryPath = outputPath + ".quantized-" + Guid.NewGuid().ToString("N") + ".png";
            try
            {
                WriteIndexedPng(
                    temporaryPath,
                    width,
                    height,
                    indices,
                    colors,
                    hasTransparency);
                File.Copy(temporaryPath, outputPath, true);
            }
            finally
            {
                if (File.Exists(temporaryPath))
                    File.Delete(temporaryPath);
            }

            PreparedAsset result = new PreparedAsset();
            result.Width = width;
            result.Height = height;
            result.UsedColors = colors.Count + (hasTransparency ? 1 : 0);
            result.HasTransparency = hasTransparency;
            result.MeanAbsoluteRgbError = meanError;
            result.MaximumRgbError = maximumError;
            return result;
        }

        private static Bitmap ResizeCover(string sourcePath, int targetWidth, int targetHeight)
        {
            using (Image source = Image.FromFile(sourcePath))
            {
                double sourceAspect = (double)source.Width / source.Height;
                double targetAspect = (double)targetWidth / targetHeight;
                RectangleF sourceRect;
                if (sourceAspect > targetAspect)
                {
                    float cropWidth = (float)(source.Height * targetAspect);
                    sourceRect = new RectangleF(
                        (source.Width - cropWidth) * 0.5f,
                        0.0f,
                        cropWidth,
                        source.Height);
                }
                else
                {
                    float cropHeight = (float)(source.Width / targetAspect);
                    sourceRect = new RectangleF(
                        0.0f,
                        (source.Height - cropHeight) * 0.5f,
                        source.Width,
                        cropHeight);
                }

                Bitmap target = new Bitmap(targetWidth, targetHeight, PixelFormat.Format32bppArgb);
                target.SetResolution(96.0f, 96.0f);
                using (Graphics graphics = Graphics.FromImage(target))
                using (ImageAttributes attributes = new ImageAttributes())
                {
                    graphics.CompositingMode = CompositingMode.SourceCopy;
                    graphics.CompositingQuality = CompositingQuality.HighQuality;
                    graphics.InterpolationMode = InterpolationMode.HighQualityBicubic;
                    graphics.PixelOffsetMode = PixelOffsetMode.HighQuality;
                    graphics.SmoothingMode = SmoothingMode.HighQuality;
                    attributes.SetWrapMode(WrapMode.TileFlipXY);
                    graphics.DrawImage(
                        source,
                        new Rectangle(0, 0, targetWidth, targetHeight),
                        sourceRect.X,
                        sourceRect.Y,
                        sourceRect.Width,
                        sourceRect.Height,
                        GraphicsUnit.Pixel,
                        attributes);
                }
                return target;
            }
        }

        private static int[] ReadPixels(
            Bitmap bitmap,
            bool flatten,
            int matteR,
            int matteG,
            int matteB)
        {
            int width = bitmap.Width;
            int height = bitmap.Height;
            int[] pixels = new int[width * height];
            Rectangle rectangle = new Rectangle(0, 0, width, height);
            BitmapData data = bitmap.LockBits(
                rectangle,
                ImageLockMode.ReadOnly,
                PixelFormat.Format32bppArgb);
            try
            {
                byte[] row = new byte[width * 4];
                for (int y = 0; y < height; ++y)
                {
                    IntPtr rowAddress = IntPtr.Add(data.Scan0, y * data.Stride);
                    Marshal.Copy(rowAddress, row, 0, row.Length);
                    int destination = y * width;
                    for (int x = 0; x < width; ++x)
                    {
                        int offset = x * 4;
                        int b = row[offset];
                        int g = row[offset + 1];
                        int r = row[offset + 2];
                        int a = row[offset + 3];
                        if (flatten)
                        {
                            r = (r * a + matteR * (255 - a) + 127) / 255;
                            g = (g * a + matteG * (255 - a) + 127) / 255;
                            b = (b * a + matteB * (255 - a) + 127) / 255;
                            a = 255;
                        }
                        pixels[destination + x] =
                            (a << 24) | (r << 16) | (g << 8) | b;
                    }
                }
            }
            finally
            {
                bitmap.UnlockBits(data);
            }
            return pixels;
        }

        private static Dictionary<int, HistogramBin> BuildHistogram(
            int[] pixels,
            bool hasTransparency)
        {
            Dictionary<int, HistogramBin> bins = new Dictionary<int, HistogramBin>();
            for (int i = 0; i < pixels.Length; ++i)
            {
                int value = pixels[i];
                int alpha = (value >> 24) & 255;
                if (hasTransparency && alpha < TransparentAlphaThreshold)
                    continue;

                int r = (value >> 16) & 255;
                int g = (value >> 8) & 255;
                int b = value & 255;
                int key = ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
                HistogramBin bin;
                if (!bins.TryGetValue(key, out bin))
                {
                    bin = new HistogramBin();
                    bin.Key = key;
                    bins.Add(key, bin);
                }
                bin.Count += 1;
                bin.SumR += r;
                bin.SumG += g;
                bin.SumB += b;
            }
            return bins;
        }

        private static List<Color> BuildPalette(
            Dictionary<int, HistogramBin> histogram,
            int colorLimit)
        {
            List<HistogramBin> initialBins = new List<HistogramBin>(histogram.Values);
            initialBins.Sort(new BinChannelComparer(0));
            List<ColorBox> boxes = new List<ColorBox>();
            boxes.Add(new ColorBox(initialBins));

            while (boxes.Count < colorLimit)
            {
                int selected = -1;
                long bestScore = -1;
                for (int i = 0; i < boxes.Count; ++i)
                {
                    if (!boxes[i].CanSplit) continue;
                    long score = boxes[i].SplitScore;
                    if (score > bestScore)
                    {
                        bestScore = score;
                        selected = i;
                    }
                }
                if (selected < 0) break;

                ColorBox box = boxes[selected];
                int rangeR = box.MaxR - box.MinR;
                int rangeG = box.MaxG - box.MinG;
                int rangeB = box.MaxB - box.MinB;
                int channel = 0;
                int weightedRange = rangeR * 2;
                if (rangeG * 4 > weightedRange)
                {
                    channel = 1;
                    weightedRange = rangeG * 4;
                }
                if (rangeB > weightedRange)
                    channel = 2;

                box.Bins.Sort(new BinChannelComparer(channel));
                long halfway = (box.Population + 1) / 2;
                long accumulated = 0;
                int split = 1;
                for (int i = 0; i < box.Bins.Count - 1; ++i)
                {
                    accumulated += box.Bins[i].Count;
                    split = i + 1;
                    if (accumulated >= halfway) break;
                }

                List<HistogramBin> left = box.Bins.GetRange(0, split);
                List<HistogramBin> right = box.Bins.GetRange(split, box.Bins.Count - split);
                boxes[selected] = new ColorBox(left);
                boxes.Insert(selected + 1, new ColorBox(right));
            }

            List<Color> palette = new List<Color>(boxes.Count);
            for (int i = 0; i < boxes.Count; ++i)
                palette.Add(Average(boxes[i].Bins));

            // Four deterministic Lloyd iterations refine the median-cut seed.
            for (int iteration = 0; iteration < 4; ++iteration)
            {
                long[] count = new long[palette.Count];
                long[] sumR = new long[palette.Count];
                long[] sumG = new long[palette.Count];
                long[] sumB = new long[palette.Count];
                foreach (HistogramBin bin in histogram.Values)
                {
                    int nearest = FindNearest(bin.R, bin.G, bin.B, palette);
                    count[nearest] += bin.Count;
                    sumR[nearest] += bin.SumR;
                    sumG[nearest] += bin.SumG;
                    sumB[nearest] += bin.SumB;
                }
                for (int i = 0; i < palette.Count; ++i)
                {
                    if (count[i] == 0) continue;
                    palette[i] = Color.FromArgb(
                        255,
                        (int)((sumR[i] + count[i] / 2) / count[i]),
                        (int)((sumG[i] + count[i] / 2) / count[i]),
                        (int)((sumB[i] + count[i] / 2) / count[i]));
                }
            }
            return palette;
        }

        private static Color Average(List<HistogramBin> bins)
        {
            long count = 0;
            long sumR = 0;
            long sumG = 0;
            long sumB = 0;
            for (int i = 0; i < bins.Count; ++i)
            {
                count += bins[i].Count;
                sumR += bins[i].SumR;
                sumG += bins[i].SumG;
                sumB += bins[i].SumB;
            }
            return Color.FromArgb(
                255,
                (int)((sumR + count / 2) / count),
                (int)((sumG + count / 2) / count),
                (int)((sumB + count / 2) / count));
        }

        private static int FindNearest(int r, int g, int b, IList<Color> palette)
        {
            int nearest = 0;
            long best = Int64.MaxValue;
            for (int i = 0; i < palette.Count; ++i)
            {
                int dr = r - palette[i].R;
                int dg = g - palette[i].G;
                int db = b - palette[i].B;
                long distance = 2L * dr * dr + 4L * dg * dg + db * db;
                if (distance < best)
                {
                    best = distance;
                    nearest = i;
                }
            }
            return nearest;
        }

        private static byte[] MapPixels(
            int[] pixels,
            int width,
            int height,
            List<Color> palette,
            bool hasTransparency,
            bool dither,
            out double meanError,
            out int maximumError)
        {
            int[] lookup = new int[32768];
            for (int r5 = 0; r5 < 32; ++r5)
            {
                int r = r5 == 31 ? 255 : (r5 << 3) + 4;
                for (int g5 = 0; g5 < 32; ++g5)
                {
                    int g = g5 == 31 ? 255 : (g5 << 3) + 4;
                    for (int b5 = 0; b5 < 32; ++b5)
                    {
                        int b = b5 == 31 ? 255 : (b5 << 3) + 4;
                        int key = (r5 << 10) | (g5 << 5) | b5;
                        lookup[key] = FindNearest(r, g, b, palette);
                    }
                }
            }

            byte[] indices = new byte[pixels.Length];
            int[] currentR = new int[width + 2];
            int[] currentG = new int[width + 2];
            int[] currentB = new int[width + 2];
            int[] nextR = new int[width + 2];
            int[] nextG = new int[width + 2];
            int[] nextB = new int[width + 2];
            long totalError = 0;
            long opaqueSamples = 0;
            maximumError = 0;
            int paletteOffset = hasTransparency ? 1 : 0;

            for (int y = 0; y < height; ++y)
            {
                bool reverse = dither && ((y & 1) != 0);
                int start = reverse ? width - 1 : 0;
                int end = reverse ? -1 : width;
                int step = reverse ? -1 : 1;
                for (int x = start; x != end; x += step)
                {
                    int pixelIndex = y * width + x;
                    int value = pixels[pixelIndex];
                    int alpha = (value >> 24) & 255;
                    if (hasTransparency && alpha < TransparentAlphaThreshold)
                    {
                        indices[pixelIndex] = 0;
                        continue;
                    }

                    int position = x + 1;
                    int sourceR = (value >> 16) & 255;
                    int sourceG = (value >> 8) & 255;
                    int sourceB = value & 255;
                    int r = ClampByte(sourceR + (dither ? DivideRound(currentR[position], 16) : 0));
                    int g = ClampByte(sourceG + (dither ? DivideRound(currentG[position], 16) : 0));
                    int b = ClampByte(sourceB + (dither ? DivideRound(currentB[position], 16) : 0));
                    int key = ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
                    int mapped = lookup[key];
                    Color color = palette[mapped];
                    indices[pixelIndex] = (byte)(mapped + paletteOffset);

                    int absoluteError =
                        Math.Abs(sourceR - color.R) +
                        Math.Abs(sourceG - color.G) +
                        Math.Abs(sourceB - color.B);
                    totalError += absoluteError;
                    opaqueSamples += 3;
                    if (absoluteError > maximumError) maximumError = absoluteError;

                    if (dither)
                    {
                        int errorR = r - color.R;
                        int errorG = g - color.G;
                        int errorB = b - color.B;
                        if (!reverse)
                        {
                            AddError(currentR, currentG, currentB, position + 1, errorR, errorG, errorB, 7);
                            AddError(nextR, nextG, nextB, position - 1, errorR, errorG, errorB, 3);
                            AddError(nextR, nextG, nextB, position, errorR, errorG, errorB, 5);
                            AddError(nextR, nextG, nextB, position + 1, errorR, errorG, errorB, 1);
                        }
                        else
                        {
                            AddError(currentR, currentG, currentB, position - 1, errorR, errorG, errorB, 7);
                            AddError(nextR, nextG, nextB, position + 1, errorR, errorG, errorB, 3);
                            AddError(nextR, nextG, nextB, position, errorR, errorG, errorB, 5);
                            AddError(nextR, nextG, nextB, position - 1, errorR, errorG, errorB, 1);
                        }
                    }
                }

                int[] swap;
                swap = currentR; currentR = nextR; nextR = swap; Array.Clear(nextR, 0, nextR.Length);
                swap = currentG; currentG = nextG; nextG = swap; Array.Clear(nextG, 0, nextG.Length);
                swap = currentB; currentB = nextB; nextB = swap; Array.Clear(nextB, 0, nextB.Length);
            }

            meanError = opaqueSamples == 0 ? 0.0 : (double)totalError / opaqueSamples;
            return indices;
        }

        private static void AddError(
            int[] red,
            int[] green,
            int[] blue,
            int position,
            int errorR,
            int errorG,
            int errorB,
            int weight)
        {
            red[position] += errorR * weight;
            green[position] += errorG * weight;
            blue[position] += errorB * weight;
        }

        private static int DivideRound(int value, int divisor)
        {
            if (value >= 0) return (value + divisor / 2) / divisor;
            return -((-value + divisor / 2) / divisor);
        }

        private static int ClampByte(int value)
        {
            if (value < 0) return 0;
            if (value > 255) return 255;
            return value;
        }

        private static void WriteIndexedPng(
            string path,
            int width,
            int height,
            byte[] indices,
            List<Color> colors,
            bool hasTransparency)
        {
            using (Bitmap indexed = new Bitmap(width, height, PixelFormat.Format8bppIndexed))
            {
                ColorPalette outputPalette = indexed.Palette;
                int offset = hasTransparency ? 1 : 0;
                if (hasTransparency)
                    outputPalette.Entries[0] = Color.FromArgb(0, 0, 0, 0);
                for (int i = 0; i < colors.Count; ++i)
                    outputPalette.Entries[i + offset] = colors[i];

                Color fill = colors[colors.Count - 1];
                for (int i = colors.Count + offset; i < outputPalette.Entries.Length; ++i)
                    outputPalette.Entries[i] = fill;
                indexed.Palette = outputPalette;

                Rectangle rectangle = new Rectangle(0, 0, width, height);
                BitmapData data = indexed.LockBits(
                    rectangle,
                    ImageLockMode.WriteOnly,
                    PixelFormat.Format8bppIndexed);
                try
                {
                    byte[] row = new byte[Math.Abs(data.Stride)];
                    for (int y = 0; y < height; ++y)
                    {
                        Array.Clear(row, 0, row.Length);
                        Buffer.BlockCopy(indices, y * width, row, 0, width);
                        IntPtr rowAddress = IntPtr.Add(data.Scan0, y * data.Stride);
                        Marshal.Copy(row, 0, rowAddress, row.Length);
                    }
                }
                finally
                {
                    indexed.UnlockBits(data);
                }
                indexed.Save(path, ImageFormat.Png);
            }
        }
    }
}
'@

    $referenceDirectory = Join-Path $PSHOME "ref"
    if (Test-Path -LiteralPath $referenceDirectory -PathType Container) {
        $drawingReferences = @(
            (Join-Path $referenceDirectory "System.Runtime.dll")
            (Join-Path $referenceDirectory "System.Collections.dll")
            (Join-Path $referenceDirectory "System.Drawing.Primitives.dll")
            (Join-Path $referenceDirectory "System.IO.FileSystem.dll")
            (Join-Path $referenceDirectory "System.Runtime.Extensions.dll")
            (Join-Path $referenceDirectory "System.Runtime.InteropServices.dll")
            [System.Drawing.Bitmap].Assembly.Location
        ) | Select-Object -Unique
    }
    else {
        # Windows PowerShell resolves the .NET Framework reference assembly by
        # its short name and supplies the standard framework references.
        $drawingReferences = @("System.Drawing")
    }
    Add-Type -TypeDefinition $quantizerSource -ReferencedAssemblies $drawingReferences
}

function Get-PngHeader {
    param([Parameter(Mandatory = $true)][string]$Path)

    $bytes = [System.IO.File]::ReadAllBytes($Path)
    $signature = @(137, 80, 78, 71, 13, 10, 26, 10)
    if ($bytes.Length -lt 29) {
        throw "PNG is too short: $Path"
    }
    for ($index = 0; $index -lt $signature.Count; $index++) {
        if ($bytes[$index] -ne $signature[$index]) {
            throw "Invalid PNG signature: $Path"
        }
    }

    $width =
        ([int]$bytes[16] -shl 24) -bor
        ([int]$bytes[17] -shl 16) -bor
        ([int]$bytes[18] -shl 8) -bor
        [int]$bytes[19]
    $height =
        ([int]$bytes[20] -shl 24) -bor
        ([int]$bytes[21] -shl 16) -bor
        ([int]$bytes[22] -shl 8) -bor
        [int]$bytes[23]

    [pscustomobject]@{
        Width       = $width
        Height      = $height
        BitDepth    = [int]$bytes[24]
        ColorType   = [int]$bytes[25]
        Compression = [int]$bytes[26]
        Filter      = [int]$bytes[27]
        Interlace   = [int]$bytes[28]
    }
}

$resolvedAssetRoot = [System.IO.Path]::GetFullPath($AssetRoot)
$sourceRoot = Join-Path $resolvedAssetRoot "source"
$productionRoot = Join-Path $resolvedAssetRoot "sce_sys"

$jobs = @(
    [pscustomobject]@{
        Name = "icon0.png"
        Source = Join-Path $sourceRoot "icon-master.png"
        Output = Join-Path $productionRoot "icon0.png"
        Width = 128
        Height = 128
        Flatten = $true
    },
    [pscustomobject]@{
        Name = "pic0.png"
        Source = Join-Path $sourceRoot "livearea-background-master.png"
        Output = Join-Path $productionRoot "pic0.png"
        Width = 960
        Height = 544
        Flatten = $false
    },
    [pscustomobject]@{
        Name = "bg.png"
        Source = Join-Path $sourceRoot "livearea-background-master.png"
        Output = Join-Path $productionRoot "livearea\contents\bg.png"
        Width = 840
        Height = 500
        Flatten = $false
    },
    [pscustomobject]@{
        Name = "startup.png"
        Source = Join-Path $sourceRoot "startup-master.png"
        Output = Join-Path $productionRoot "livearea\contents\startup.png"
        Width = 280
        Height = 158
        Flatten = $false
    }
)

$masterHashes = @{}
foreach ($source in ($jobs.Source | Sort-Object -Unique)) {
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw "Missing source master: $source"
    }
    $masterHashes[$source] = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
}

$results = @()
foreach ($job in $jobs) {
    Write-Host "Preparing $($job.Name) from $([System.IO.Path]::GetFileName($job.Source))..."
    $prepared = [VitaDevDeploy.LiveArea.AssetPipeline]::Prepare(
        $job.Source,
        $job.Output,
        $job.Width,
        $job.Height,
        $MaxColors,
        $job.Flatten,
        7,
        20,
        38,
        -not $DisableDithering)

    $header = Get-PngHeader -Path $job.Output
    if ($header.Width -ne $job.Width -or $header.Height -ne $job.Height) {
        throw "Wrong dimensions in $($job.Output): $($header.Width)x$($header.Height)"
    }
    if ($header.BitDepth -ne 8 -or $header.ColorType -ne 3) {
        throw "Production asset is not an 8-bit indexed PNG: $($job.Output)"
    }
    if ($header.Compression -ne 0 -or $header.Filter -ne 0 -or $header.Interlace -ne 0) {
        throw "Production asset uses unsupported PNG encoding fields: $($job.Output)"
    }

    $bitmap = [System.Drawing.Bitmap]::FromFile($job.Output)
    try {
        if ($bitmap.PixelFormat -ne [System.Drawing.Imaging.PixelFormat]::Format8bppIndexed) {
            throw "System.Drawing did not decode $($job.Output) as Format8bppIndexed."
        }
        if ($job.Flatten) {
            $transparentEntries = @($bitmap.Palette.Entries | Where-Object { $_.A -ne 255 })
            if ($transparentEntries.Count -ne 0) {
                throw "Flattened icon contains transparent palette entries: $($job.Output)"
            }
        }
    }
    finally {
        $bitmap.Dispose()
    }

    $results += [pscustomobject]@{
        Asset = $job.Name
        Dimensions = "$($prepared.Width)x$($prepared.Height)"
        Colors = $prepared.UsedColors
        Transparent = $prepared.HasTransparency
        MeanRgbError = [Math]::Round($prepared.MeanAbsoluteRgbError, 2)
        MaxRgbError = $prepared.MaximumRgbError
        Bytes = (Get-Item -LiteralPath $job.Output).Length
        SHA256 = (Get-FileHash -LiteralPath $job.Output -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}

foreach ($source in $masterHashes.Keys) {
    $afterHash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
    if ($afterHash -ne $masterHashes[$source]) {
        throw "Source master was modified: $source"
    }
}

$results | Format-Table -AutoSize
Write-Host "LiveArea assets are ready: indexed color, 8-bit, non-interlaced; source masters unchanged."
