#include "uvdb_thread_control.h"

#include <limits.h>
#include <string.h>

#define UVDB_VCONT_MAX_ACTIONS (UVDB_THREAD_INVENTORY_CAPACITY + 1u)

struct uvdb_vcont_action {
    enum uvdb_resume_kind kind;
    int32_t thread;
    unsigned int has_thread;
};

void uvdb_thread_inventory_reset(struct uvdb_thread_inventory* inventory)
{
    if(inventory)
        memset(inventory, 0, sizeof(*inventory));
}

int uvdb_thread_inventory_contains(
    const struct uvdb_thread_inventory* inventory,
    int32_t id)
{
    if(!inventory || inventory->count > UVDB_THREAD_INVENTORY_CAPACITY ||
       id <= 0)
        return 0;
    for(size_t i = 0; i < inventory->count; ++i)
        if(inventory->ids[i] == id)
            return 1;
    return 0;
}

int uvdb_thread_inventory_add(struct uvdb_thread_inventory* inventory,
                              int32_t id)
{
    if(!inventory || inventory->count > UVDB_THREAD_INVENTORY_CAPACITY ||
       id <= 0)
        return -1;
    if(uvdb_thread_inventory_contains(inventory, id))
        return 0;
    if(inventory->count >= UVDB_THREAD_INVENTORY_CAPACITY)
        return -1;
    inventory->ids[inventory->count++] = id;
    return 1;
}

void uvdb_thread_selection_reset(struct uvdb_thread_selection* selection)
{
    if(!selection)
        return;
    selection->stopped = UVDB_RSP_THREAD_ALL;
    selection->general = UVDB_RSP_THREAD_ANY;
    selection->resume = UVDB_RSP_THREAD_ALL;
}

void uvdb_thread_selection_reconcile(
    struct uvdb_thread_selection* selection,
    const struct uvdb_thread_inventory* inventory)
{
    if(!selection || !inventory)
        return;
    if(selection->stopped > 0 &&
       !uvdb_thread_inventory_contains(inventory, selection->stopped))
        selection->stopped = UVDB_RSP_THREAD_ALL;
    /*
     * Keep explicit Hg/Hc selectors even after their thread disappears.  The
     * next operation then fails closed instead of silently widening a stale
     * per-thread request to the stopped thread or the whole process.
     */
}

void uvdb_thread_selection_note_stop(
    struct uvdb_thread_selection* selection,
    int32_t stopped_thread,
    const struct uvdb_thread_inventory* inventory)
{
    if(!selection)
        return;
    selection->stopped = stopped_thread > 0 ? stopped_thread
                                            : UVDB_RSP_THREAD_ALL;
    if(inventory)
        uvdb_thread_selection_reconcile(selection, inventory);
}

void uvdb_thread_selection_note_resume(
    struct uvdb_thread_selection* selection)
{
    if(selection)
        selection->stopped = UVDB_RSP_THREAD_ALL;
}

static int hex_value(char value)
{
    if(value >= '0' && value <= '9')
        return value - '0';
    if(value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    if(value >= 'A' && value <= 'F')
        return value - 'A' + 10;
    return -1;
}

int uvdb_rsp_parse_u32_hex(const char* text, size_t size, uint32_t* value)
{
    if(!text || !value || !size || size > 8)
        return -1;

    uint32_t parsed = 0;
    for(size_t i = 0; i < size; ++i)
    {
        int digit = hex_value(text[i]);
        if(digit < 0)
            return -1;
        if(parsed > (UINT32_MAX - (uint32_t)digit) / 16u)
            return -1;
        parsed = parsed * 16u + (uint32_t)digit;
    }
    *value = parsed;
    return 0;
}

int uvdb_rsp_parse_thread_id(const char* text, size_t size, int32_t* id)
{
    if(!text || !id || !size)
        return -1;
    if(size == 2 && text[0] == '-' && text[1] == '1')
    {
        *id = UVDB_RSP_THREAD_ALL;
        return 0;
    }

    uint32_t value;
    if(uvdb_rsp_parse_u32_hex(text, size, &value) < 0 || value > INT32_MAX)
        return -1;
    *id = (int32_t)value;
    return 0;
}

int uvdb_thread_selection_apply(
    struct uvdb_thread_selection* selection,
    char operation,
    const char* text,
    size_t size,
    const struct uvdb_thread_inventory* inventory)
{
    if(!selection || !inventory ||
       (operation != 'g' && operation != 'c'))
        return -1;
    int32_t id;
    if(uvdb_rsp_parse_thread_id(text, size, &id) < 0)
        return -1;
    if(id > 0 && !uvdb_thread_inventory_contains(inventory, id))
        return -1;
    if(operation == 'g')
        selection->general = id;
    else
        selection->resume = id;
    return 0;
}

static int32_t resolve_selector(
    int32_t selector,
    const struct uvdb_thread_selection* selection,
    const struct uvdb_thread_inventory* inventory)
{
    if(selector > 0 && uvdb_thread_inventory_contains(inventory, selector))
        return selector;
    if(selector > 0)
        return UVDB_RSP_THREAD_ALL;
    if(selection && selection->stopped > 0 &&
       uvdb_thread_inventory_contains(inventory, selection->stopped))
        return selection->stopped;
    if(inventory && inventory->count)
        return inventory->ids[0];
    return UVDB_RSP_THREAD_ALL;
}

int32_t uvdb_thread_selection_general(
    const struct uvdb_thread_selection* selection,
    const struct uvdb_thread_inventory* inventory)
{
    if(!selection || !inventory)
        return UVDB_RSP_THREAD_ALL;
    return resolve_selector(selection->general, selection, inventory);
}

int32_t uvdb_thread_selection_step(
    const struct uvdb_thread_selection* selection,
    const struct uvdb_thread_inventory* inventory)
{
    if(!selection || !inventory)
        return UVDB_RSP_THREAD_ALL;
    return resolve_selector(selection->resume, selection, inventory);
}

int uvdb_thread_selection_plan_legacy(
    const struct uvdb_thread_selection* selection,
    const struct uvdb_thread_inventory* inventory,
    int stepping,
    struct uvdb_resume_plan* plan)
{
    if(!selection || !inventory || !plan || inventory->count == 0 ||
       inventory->count > UVDB_THREAD_INVENTORY_CAPACITY)
        return -1;
    if(selection->resume > 0)
        return -1;
    if(stepping && selection->resume == UVDB_RSP_THREAD_ALL &&
       inventory->count > 1)
        return -1;

    int32_t selected = uvdb_thread_selection_step(selection, inventory);
    if(selected <= 0)
        return -1;
    plan->kind = stepping ? UVDB_RESUME_STEP : UVDB_RESUME_CONTINUE;
    plan->step_thread = stepping ? selected : UVDB_RSP_THREAD_ALL;
    return 0;
}

static int parse_vcont_action(
    const char* text,
    size_t size,
    struct uvdb_vcont_action* action)
{
    if(!text || !size || !action)
        return -1;
    if(text[0] == 'c')
        action->kind = UVDB_RESUME_CONTINUE;
    else if(text[0] == 's')
        action->kind = UVDB_RESUME_STEP;
    else
        return -1;

    action->thread = UVDB_RSP_THREAD_ALL;
    action->has_thread = 0;
    if(size == 1)
        return 0;
    if(size < 3 || text[1] != ':')
        return -1;
    if(uvdb_rsp_parse_thread_id(text + 2, size - 2, &action->thread) < 0)
        return -1;
    action->has_thread = 1;
    return 0;
}

static int action_matches(
    const struct uvdb_vcont_action* action,
    int32_t thread,
    int32_t any_thread)
{
    if(!action->has_thread || action->thread == UVDB_RSP_THREAD_ALL)
        return 1;
    if(action->thread == UVDB_RSP_THREAD_ANY)
        return thread == any_thread;
    return action->thread == thread;
}

int uvdb_rsp_parse_vcont(
    const char* packet,
    size_t size,
    const struct uvdb_thread_inventory* inventory,
    struct uvdb_resume_plan* plan)
{
    static const char prefix[] = "vCont;";
    if(!packet || !inventory || !plan || inventory->count == 0 ||
       inventory->count > UVDB_THREAD_INVENTORY_CAPACITY ||
       size <= sizeof(prefix) - 1 ||
       memcmp(packet, prefix, sizeof(prefix) - 1) != 0)
        return -1;

    struct uvdb_vcont_action actions[UVDB_VCONT_MAX_ACTIONS];
    size_t action_count = 0;
    size_t cursor = sizeof(prefix) - 1;
    while(cursor < size)
    {
        size_t end = cursor;
        while(end < size && packet[end] != ';')
            ++end;
        if(end == cursor || action_count >= UVDB_VCONT_MAX_ACTIONS ||
           parse_vcont_action(packet + cursor, end - cursor,
                              &actions[action_count]) < 0)
            return -1;
        if(actions[action_count].has_thread &&
           actions[action_count].thread > 0 &&
           !uvdb_thread_inventory_contains(inventory,
                                            actions[action_count].thread))
            return -1;
        ++action_count;
        if(end == size)
            break;
        cursor = end + 1;
        if(cursor == size)
            return -1;
    }

    size_t step_count = 0;
    int32_t step_thread = UVDB_RSP_THREAD_ALL;
    int32_t any_thread = inventory->ids[0];
    for(size_t thread_index = 0; thread_index < inventory->count;
        ++thread_index)
    {
        int matched = 0;
        for(size_t action_index = 0; action_index < action_count;
            ++action_index)
        {
            if(!action_matches(&actions[action_index],
                               inventory->ids[thread_index], any_thread))
                continue;
            matched = 1;
            if(actions[action_index].kind == UVDB_RESUME_STEP)
            {
                ++step_count;
                step_thread = inventory->ids[thread_index];
            }
            break;
        }
        if(!matched || step_count > 1)
            return -1;
    }

    plan->kind = step_count ? UVDB_RESUME_STEP : UVDB_RESUME_CONTINUE;
    plan->step_thread = step_thread;
    return 0;
}

int uvdb_stop_cleanup_can_release(int coherent_stop, int breakpoints_active)
{
    return coherent_stop || !breakpoints_active;
}
