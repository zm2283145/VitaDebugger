#include "vitaprofiler_pmu_vita.h"

int main(void)
{
    struct vp_vita_pmu_owned owned = {0};
    int result = vp_vita_pmu_owned_init(
        &owned, 0, VP_VITA_PMU_APPLICATION_OWNERSHIP_ACK);
    if (result != VP_RESULT_OK)
        return 1;
    return vp_vita_pmu_owned_get_provider(&owned) == 0 ? 2 : 0;
}
