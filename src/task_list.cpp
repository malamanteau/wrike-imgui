#include <jsmn.h>
#include <cstdio>
#include <imgui.h>
#include "id_hash_map.h"
#include "json.h"
#include "temporary_storage.h"
#include "accounts.h"
#include "main.h"
#include "lazy_array.h"
#include "platform.h"
#include "users.h"
#include "workflows.h"
#include "task_view.h"
#include "renderer.h"
#include "ui.h"

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui_internal.h>

enum Task_List_Sort_Field {
    Task_List_Sort_Field_None,
    Task_List_Sort_Field_Title,
    Task_List_Sort_Field_Status,
    Task_List_Sort_Field_Assignee,
    Task_List_Sort_Field_Custom_Field
};

struct Folder_Task {
    Task_Id id;
    Custom_Status_Id custom_status_id;
    u32 custom_status_id_hash;

    String title;
    Relative_Pointer<Custom_Field_Value> custom_field_values;
    u32 num_custom_field_values;

    Relative_Pointer<Folder_Id> parent_folder_ids;
    u32 num_parent_folder_ids;

    Relative_Pointer<Task_Id> parent_task_ids;
    u32 num_parent_task_ids;

    Relative_Pointer<User_Id> assignees;
    u32 num_assignees;
};

struct Folder_Header {
    Folder_Id id;
    String name;
    Custom_Field_Id* custom_columns;
    u32 num_custom_columns;
};

struct Sorted_Folder_Task {
    Task_Id id;
    u32 id_hash;

    Folder_Task* source_task;
    Custom_Status* cached_status;
    User* cached_first_assignee;
    Sorted_Folder_Task** sub_tasks; // TODO Array<Sorted_Folder_Task*>
    u32 num_sub_tasks;

    bool is_expanded;
};

struct Flattened_Folder_Task {
    Sorted_Folder_Task* sorted_task;
    u32 nesting_level;
    u32 num_visible_sub_tasks;

    bool needs_sub_task_sort;
};

struct Table_Paint_Context {
    ImDrawList* draw_list;
    Custom_Field** column_to_custom_field;
    u32 total_columns;
    float row_height;
    float scale;
    float text_padding_y;
};

typedef int (Comparator)(const void*, const void*);

static const u32 custom_columns_start_index = 3;

static Folder_Header current_folder{};

static Array<Folder_Task> folder_tasks{};
static Sorted_Folder_Task* sorted_folder_tasks = NULL;
static Array<Flattened_Folder_Task> flattened_sorted_folder_task_tree{};
static Lazy_Array<Sorted_Folder_Task*, 32> top_level_tasks{};

static Id_Hash_Map<Task_Id, Sorted_Folder_Task*> id_to_sorted_folder_task{};

static Lazy_Array<Custom_Field_Value, 16> custom_field_values{};
static Lazy_Array<Task_Id, 16> parent_task_ids{};
static Lazy_Array<User_Id, 16> assignee_ids{};
static Sorted_Folder_Task** sub_tasks = NULL;

typedef char Sort_Direction;
static const Sort_Direction Sort_Direction_Normal = 1;
static const Sort_Direction Sort_Direction_Reverse = -1;

static Task_List_Sort_Field sort_field = Task_List_Sort_Field_None;
static Custom_Field_Id sort_custom_field_id{};
static Custom_Field* sort_custom_field;
static Sort_Direction sort_direction = Sort_Direction_Normal;
static bool has_been_sorted_after_loading = false;
static bool show_only_active_tasks = true;
static bool queue_flattened_tree_rebuild = false;

static inline int compare_tasks_custom_fields(Folder_Task* a, Folder_Task* b, Custom_Field_Type custom_field_type) {
    String* a_value = NULL;
    String* b_value = NULL;

    // TODO we could cache that to sort big lists faster
    for (u32 index = 0; index < a->num_custom_field_values; index++) {
        if (a->custom_field_values[index].field_id == sort_custom_field_id) {
            a_value = &a->custom_field_values[index].value;
            break;
        }
    }

    if (!a_value) {
        return 1;
    }

    for (u32 index = 0; index < b->num_custom_field_values; index++) {
        if (b->custom_field_values[index].field_id == sort_custom_field_id) {
            b_value = &b->custom_field_values[index].value;
            break;
        }
    }

    if (!b_value) {
        return -1;
    }

    switch (custom_field_type) {
        case Custom_Field_Type_Numeric: {
            return string_atoi(a_value) - string_atoi(b_value);
        }

        case Custom_Field_Type_DropDown:
        case Custom_Field_Type_Text: {
            return strncmp(a_value->start, b_value->start, MIN(a_value->length, b_value->length));
        }

        default: {
            return 0;
        }
    }
}

static int compare_folder_tasks_based_on_title(const void* ap, const void* bp) {
    Sorted_Folder_Task* as = *(Sorted_Folder_Task**) ap;
    Sorted_Folder_Task* bs = *(Sorted_Folder_Task**) bp;

    Folder_Task* a = as->source_task;
    Folder_Task* b = bs->source_task;

    int result = strncmp(a->title.start, b->title.start, MIN(a->title.length, b->title.length)) * sort_direction;

    if (result == 0) {
        return (int) (as->source_task->id - bs->source_task->id);
    }

    return result;
}

static int compare_folder_tasks_based_on_assignees(const void* ap, const void* bp) {
    Sorted_Folder_Task* as = *(Sorted_Folder_Task**) ap;
    Sorted_Folder_Task* bs = *(Sorted_Folder_Task**) bp;

    User* a_assignee = as->cached_first_assignee;

    if (!a_assignee) {
        return 1;
    }

    User* b_assignee = bs->cached_first_assignee;

    if (!b_assignee) {
        return -1;
    }

    temporary_storage_mark();

    String a_name = full_user_name_to_temporary_string(a_assignee);
    String b_name = full_user_name_to_temporary_string(b_assignee);

    s32 result = strncmp(a_name.start, b_name.start, MIN(a_name.length, b_name.length)) * sort_direction;

    temporary_storage_reset();

    if (result == 0) {
        return (int) (as->source_task->id - bs->source_task->id);
    }

    return result;
}

static int compare_folder_tasks_based_on_status(const void* ap, const void* bp) {
    Sorted_Folder_Task* as = *(Sorted_Folder_Task**) ap;
    Sorted_Folder_Task* bs = *(Sorted_Folder_Task**) bp;

    Custom_Status* a_status = as->cached_status;
    Custom_Status* b_status = bs->cached_status;

    // TODO do status comparison based on status type?

    s32 result = a_status->natural_index - b_status->natural_index;

    if (!result) {
        result = (a_status->id - b_status->id) * sort_direction;
    }

    if (!result) {
        return (int) (as->source_task->id - bs->source_task->id);
    }

    return result;
}

static int compare_folder_tasks_based_on_custom_field(const void* ap, const void* bp) {
    Sorted_Folder_Task* as = *(Sorted_Folder_Task**) ap;
    Sorted_Folder_Task* bs = *(Sorted_Folder_Task**) bp;

    Folder_Task* a = as->source_task;
    Folder_Task* b = bs->source_task;

    // TODO inline that?
    s32 result = compare_tasks_custom_fields(a, b, sort_custom_field->type) * sort_direction;

    if (!result) {
        return (int) (as->source_task->id - bs->source_task->id);
    }

    return result;
}

static inline Comparator* get_comparator_by_current_sort_type() {
    switch (sort_field) {
        case Task_List_Sort_Field_Title: {
            return compare_folder_tasks_based_on_title;
        }

        case Task_List_Sort_Field_Assignee: {
            return compare_folder_tasks_based_on_assignees;
        }

        case Task_List_Sort_Field_Status: {
            return compare_folder_tasks_based_on_status;
        }

        case Task_List_Sort_Field_Custom_Field: {
            return compare_folder_tasks_based_on_custom_field;
        }

        default: {}
    }

    assert(!"Invalid sort field");

    return NULL;
}

static u32 rebuild_flattened_task_tree_hierarchically(Sorted_Folder_Task* task, bool is_parent_expanded, u32 level, Flattened_Folder_Task** current_task) {
    if (show_only_active_tasks && task->cached_status->group != Status_Group_Active) {
        return 0;
    }

    if (is_parent_expanded) {
        Flattened_Folder_Task* flattened_task = *current_task;
        flattened_task->sorted_task = task;
        flattened_task->nesting_level = level;
        flattened_task->num_visible_sub_tasks = 0;
        flattened_task->needs_sub_task_sort = true;

        *current_task = flattened_task + 1;

        for (u32 sub_task_index = 0; sub_task_index < task->num_sub_tasks; sub_task_index++) {
            flattened_task->num_visible_sub_tasks += rebuild_flattened_task_tree_hierarchically(task->sub_tasks[sub_task_index], task->is_expanded, level + 1, current_task);
        }
    }

    return 1;
}

static void rebuild_flattened_task_tree() {
    Flattened_Folder_Task* current_task = flattened_sorted_folder_task_tree.data;

    for (u32 task_index = 0; task_index < top_level_tasks.length; task_index++) {
        rebuild_flattened_task_tree_hierarchically(top_level_tasks[task_index], true, 0, &current_task);
    }

    flattened_sorted_folder_task_tree.length = (u32) (current_task - flattened_sorted_folder_task_tree.data);
}

static void rebuild_flattened_task_subtree(Flattened_Folder_Task* starting_at) {
    rebuild_flattened_task_tree_hierarchically(starting_at->sorted_task, true, starting_at->nesting_level, &starting_at);
}

static void sort_sub_tasks_of_task(Sorted_Folder_Task* task) {
    qsort(task->sub_tasks, task->num_sub_tasks, sizeof(Sorted_Folder_Task*), get_comparator_by_current_sort_type());
}

static void sort_top_level_tasks_and_rebuild_flattened_tree() {
    qsort(top_level_tasks.data, top_level_tasks.length, sizeof(Sorted_Folder_Task*), get_comparator_by_current_sort_type());

    rebuild_flattened_task_tree();
}

static void update_cached_data_for_sorted_tasks() {
    // TODO We actually only need to do that once when tasks/workflows combination changes, not for every sort
    for (u32 index = 0; index < folder_tasks.length; index++) {
        Sorted_Folder_Task* sorted_folder_task = &sorted_folder_tasks[index];
        Folder_Task* source = sorted_folder_task->source_task;

        sorted_folder_task->cached_status = find_custom_status_by_id(source->custom_status_id, source->custom_status_id_hash);

        if (source->num_assignees) {
            sorted_folder_task->cached_first_assignee = find_user_by_id(source->assignees[0]);
        } else {
            sorted_folder_task->cached_first_assignee = NULL;
        }
    }
}

static void sort_by_field(Task_List_Sort_Field sort_by) {
    assert(sort_by != Task_List_Sort_Field_Custom_Field);

    if (sort_field == sort_by) {
        sort_direction *= -1;
    } else {
        sort_direction = Sort_Direction_Normal;
    }

    update_cached_data_for_sorted_tasks();

    sort_field = sort_by;

    u64 start = platform_get_app_time_precise();
    sort_top_level_tasks_and_rebuild_flattened_tree();
    printf("Sorting %i elements by %i took %fms\n", folder_tasks.length, sort_by, platform_get_delta_time_ms(start));
}

static void sort_by_custom_field(Custom_Field_Id field_id) {
    if (sort_field == Task_List_Sort_Field_Custom_Field && field_id == sort_custom_field_id) {
        sort_direction *= -1;
    } else {
        sort_direction = Sort_Direction_Normal;
    }

    update_cached_data_for_sorted_tasks();

    sort_field = Task_List_Sort_Field_Custom_Field;
    sort_custom_field_id = field_id;
    sort_custom_field = find_custom_field_by_id(field_id, hash_id(field_id)); // TODO hash cache?

    u64 start = platform_get_app_time_precise();
    sort_top_level_tasks_and_rebuild_flattened_tree();
    printf("Sorting %i elements by %i took %fms\n", folder_tasks.length, field_id, platform_get_delta_time_ms(start));
}

Custom_Field** map_columns_to_custom_fields() {
    Custom_Field** column_to_custom_field = (Custom_Field**) talloc(sizeof(Custom_Field*) * current_folder.num_custom_columns);

    for (u32 column = 0; column < current_folder.num_custom_columns; column++) {
        Custom_Field_Id custom_field_id = current_folder.custom_columns[column];

        // TODO hash cache!
        Custom_Field* custom_field = find_custom_field_by_id(custom_field_id, hash_id(custom_field_id));

        column_to_custom_field[column] = custom_field;
    }

    return column_to_custom_field;
}

Custom_Field_Value* try_find_custom_field_value_in_task(Folder_Task* task, Custom_Field* field) {
    if (!field) return NULL;

    for (u32 index = 0; index < task->num_custom_field_values; index++) {
        Custom_Field_Value* value = &task->custom_field_values[index];

        if (value->field_id == field->id) {
            return value;
        }
    }

    return NULL;
}

void draw_assignees_cell_contents(ImDrawList* draw_list, Folder_Task* task, ImVec2 text_position) {
    for (u32 assignee_index = 0; assignee_index < task->num_assignees; assignee_index++) {
        User_Id user_id = task->assignees[assignee_index];
        User* user = find_user_by_id(user_id);

        if (!user) {
            continue;
        }

        bool is_not_last = assignee_index < task->num_assignees - 1;
        const char* name_pattern = "%.*s %.*s";

        if (is_not_last) {
            name_pattern = "%.*s %.*s, ";
        }

        char* start, *end;

        tprintf(name_pattern, &start, &end,
                user->first_name.length, user->first_name.start,
                user->last_name.length, user->last_name.start);

        float text_width = ImGui::CalcTextSize(start, end).x;

        draw_list->AddText(text_position, color_black_text_on_white, start, end);

        text_position.x += text_width;
    }
}

bool draw_open_task_button(Table_Paint_Context& context, ImVec2 cell_top_left, float column_width) {
    ImVec2 button_size(30.0f * context.scale, context.row_height);
    ImVec2 top_left = cell_top_left + ImVec2(column_width, 0) - ImVec2(button_size.x, 0);
    ImVec2 bottom_right = top_left + button_size;

    Button_State button_state = button("task_open_button", top_left, button_size);

    if (button_state.clipped) {
        return button_state.pressed;
    }

    ImVec2 icon_size{ button_size.x / 3.5f, context.row_height / 4.0f };
    ImVec2 icon_top_left = top_left + button_size / 2.0f - icon_size / 2.0f;
    ImVec2 icon_bottom_right = icon_top_left + icon_size;
    ImVec2 icon_bottom_left = icon_top_left + ImVec2(0.0f, icon_size.y);
    ImVec2 icon_secondary_offset = ImVec2(-2.0f, 1.5f) * context.scale;

    u32 color = button_state.hovered ? color_link : color_black_text_on_white;

    context.draw_list->AddRectFilled(top_left, bottom_right, IM_COL32_WHITE);
    context.draw_list->AddLine(icon_top_left + icon_secondary_offset, icon_bottom_left + icon_secondary_offset, color, 1.5f);
    context.draw_list->AddLine(icon_bottom_left + icon_secondary_offset, icon_bottom_right + icon_secondary_offset, color, 1.5f);
    context.draw_list->AddRect(icon_top_left, icon_bottom_right, color, 0, ImDrawCornerFlags_All, 1.5f);

    return button_state.pressed;
}

void draw_table_cell_for_task(Table_Paint_Context& context, u32 column, float column_width, Flattened_Folder_Task* flattened_task, ImVec2 cell_top_left) {
    ImVec2 padding(context.scale * 8.0f, context.text_padding_y);
    Sorted_Folder_Task* sorted_task = flattened_task->sorted_task;
    Folder_Task* task = sorted_task->source_task;

    switch (column) {
        case 0: {
            float nesting_level_padding = flattened_task->nesting_level * 20.0f * context.scale;

            if (flattened_task->num_visible_sub_tasks) {
                ImVec2 arrow_point = cell_top_left + ImVec2(context.scale * 20.0f + nesting_level_padding, context.row_height / 2.0f);

                if (draw_expand_arrow_button(context.draw_list, arrow_point, context.row_height, sorted_task->is_expanded)) {
                    sorted_task->is_expanded = !sorted_task->is_expanded;
                    /* TODO We don't need to rebuild the whole tree there
                     * TODO     this is just inserting/removing subtask tree after the current task and could be done
                     * TODO     with a simple subtree traversal to determine subtree size, then a subsequent list insert
                     * TODO     or a simple memcpy in case of a removal
                     * TODO Also queuing there is not really necessary, since the only part of the flattened array
                     * TODO     which changes lies after the current element but the current solution feels cleaner,
                     * TODO     although it delays the update for one frame
                     */
                    queue_flattened_tree_rebuild = true;
                }
            }

            ImVec2 title_padding(context.scale * 40.0f + nesting_level_padding, context.text_padding_y);

            char* start = task->title.start, * end = task->title.start + task->title.length;

            context.draw_list->AddText(cell_top_left + title_padding, color_black_text_on_white, start, end);

            if (ImGui::IsMouseHoveringRect(cell_top_left, cell_top_left + ImVec2(column_width, context.row_height))) {
                if (draw_open_task_button(context, cell_top_left, column_width)) {
                    request_task_by_task_id(task->id);
                }
            }

            break;
        }

        case 1: {
            Custom_Status* status = sorted_task->cached_status;

            if (status) {
                char* start = status->name.start, * end = status->name.start + status->name.length;

                context.draw_list->AddText(cell_top_left + padding, status->color, start, end);
            }

            break;
        }

        case 2: {
            ImVec2 text_position = cell_top_left + padding;

            draw_assignees_cell_contents(context.draw_list, task, text_position);

            break;
        }

        default: {
            if (column > custom_columns_start_index) {
                Custom_Field* custom_field = context.column_to_custom_field[column - custom_columns_start_index];
                Custom_Field_Value* field_value = try_find_custom_field_value_in_task(task, custom_field);

                if (field_value) {
                    char* start = field_value->value.start, * end = field_value->value.start + field_value->value.length;

                    context.draw_list->AddText(cell_top_left + padding, color_black_text_on_white, start, end);
                }
            }
        }
    }
}

float get_column_width(Table_Paint_Context& context, u32 column) {
    float column_width = 50.0f;

    if (column == 0) {
        column_width = 500.0f;
    } else if (column == 1) {
        column_width = 200.0f;
    } else if (column == 2) {
        column_width = 200.0f;
    }

    return column_width * context.scale;
}

String get_column_title(Table_Paint_Context& context, u32 column) {
    String column_title{};

    switch (column) {
        case 0: column_title.start = (char*) "Title"; break;
        case 1: column_title.start = (char*) "Status"; break;
        case 2: column_title.start = (char*) "Assignees"; break;

        default: {
            Custom_Field* custom_field = context.column_to_custom_field[column - custom_columns_start_index];
            column_title = custom_field->title;
        }
    }

    if (!column_title.length) {
        column_title.length = (u32) strlen(column_title.start);
    }

    return column_title;
}

Task_List_Sort_Field get_column_sort_field(u32 column) {
    switch (column) {
        case 0: return Task_List_Sort_Field_Title;
        case 1: return Task_List_Sort_Field_Status;
        case 2: return Task_List_Sort_Field_Assignee;

        default: {
            return Task_List_Sort_Field_Custom_Field;
        }
    }
}

void draw_folder_header(Table_Paint_Context& context, float content_width) {
    ImVec2 top_left = ImGui::GetCursorScreenPos();

    float toolbar_height = 24.0f * context.scale;
    float folder_header_height = 56.0f * context.scale;

    ImGui::Dummy({ 0, folder_header_height + toolbar_height});

    ImGui::PushFont(font_28px);

    ImVec2 folder_header_padding = ImVec2(16.0f * context.scale, folder_header_height / 2.0f - ImGui::GetFontSize() / 2.0f);

    if (current_folder.name.start) {
        context.draw_list->AddText(top_left + folder_header_padding, color_black_text_on_white,
                                   current_folder.name.start, current_folder.name.start + current_folder.name.length);
    }

    ImGui::PopFont();

    const u32 toolbar_background = 0xfff7f7f7;

    ImVec2 toolbar_top_left = top_left + ImVec2(0, folder_header_height);
    ImVec2 toolbar_bottom_right = toolbar_top_left + ImVec2(content_width, toolbar_height);

    context.draw_list->AddRectFilled(toolbar_top_left, toolbar_bottom_right, toolbar_background);
}

void draw_table_header(Table_Paint_Context& context, ImVec2 window_top_left) {
    float column_left_x = 0.0f;
    ImDrawList* draw_list = context.draw_list;

    for (u32 column = 0; column < context.total_columns; column++) {
        float column_width = get_column_width(context, column);
        String column_title = get_column_title(context, column);

        char* start, * end;

        start = column_title.start;
        end = column_title.start + column_title.length;

        ImVec2 column_top_left_absolute = window_top_left + ImVec2(column_left_x, 0) + ImVec2(0, ImGui::GetScrollY());
        ImVec2 size { column_width, context.row_height };

        ImGui::PushID(column);

        Button_State button_state = button("header_sort_button", column_top_left_absolute, size);

        ImGui::PopID();

        bool sorting_by_this_column;

        {
            Task_List_Sort_Field column_sort_field = get_column_sort_field(column);

            if (column_sort_field == Task_List_Sort_Field_Custom_Field) {
                Custom_Field* column_custom_field = context.column_to_custom_field[column - custom_columns_start_index];

                if (button_state.pressed) {
                    sort_by_custom_field(column_custom_field->id);
                }

                sorting_by_this_column = sort_custom_field_id == column_custom_field->id;
            } else {
                if (button_state.pressed) {
                    sort_by_field(column_sort_field);
                }

                sorting_by_this_column = column_sort_field == sort_field;
            }
        }

        if (button_state.clipped) {
            column_left_x += column_width;

            continue;
        }

        u32 text_color = button_state.hovered ? color_link : color_black_text_on_white;

        const u32 grid_color = 0xffebebeb;

        draw_list->AddRectFilled(column_top_left_absolute, column_top_left_absolute + size, IM_COL32_WHITE);
        draw_list->AddText(column_top_left_absolute + ImVec2(8.0f * context.scale, context.text_padding_y), text_color, start, end);
        draw_list->AddLine(column_top_left_absolute, column_top_left_absolute + ImVec2(0, context.row_height), grid_color, 1.25f);

        if (sorting_by_this_column) {
            // TODO draw sort direction arrow
        }

        column_left_x += column_width;
    }
}

void draw_task_list() {
    ImGuiID task_list_id = ImGui::GetID("task_list");
    ImGui::BeginChildFrame(task_list_id, ImVec2(-1, -1));

    const bool is_folder_data_loading = folder_contents_request != NO_REQUEST || folder_header_request != NO_REQUEST;
    const bool are_users_loading = contacts_request != NO_REQUEST;
    const bool are_custom_fields_loading = accounts_request != NO_REQUEST;

    if (!is_folder_data_loading && custom_statuses_were_loaded && !are_users_loading && !are_custom_fields_loading) {
        if (!has_been_sorted_after_loading) {
            sort_by_field(Task_List_Sort_Field_Title);
            has_been_sorted_after_loading = true;
        }

        const u32 grid_color = 0xffebebeb;
        const float scale = platform_get_pixel_ratio();
        const float row_height = 24.0f * scale;

        Table_Paint_Context paint_context;
        paint_context.scale = scale;
        paint_context.draw_list = ImGui::GetWindowDrawList();
        paint_context.row_height = row_height;
        paint_context.text_padding_y = row_height / 2.0f - ImGui::GetFontSize() / 2.0f;
        paint_context.column_to_custom_field = map_columns_to_custom_fields();
        paint_context.total_columns = current_folder.num_custom_columns + custom_columns_start_index;

        draw_folder_header(paint_context, ImGui::GetWindowWidth());

        ImGui::BeginChild("table_content", ImVec2(-1, -1), false, ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::PushFont(font_19px);

        ImDrawList* draw_list = ImGui::GetWindowDrawList();

        paint_context.draw_list = draw_list;

        const float content_width = ImGui::GetWindowWidth();
        const float content_height = ImGui::GetWindowHeight();

        const ImVec2 window_top_left = ImGui::GetCursorScreenPos();
        const ImVec2 window_bottom_right_no_scroll = ImGui::GetWindowPos() + ImVec2(content_width, content_height);

        float column_left_x = 0.0f;

        u32 first_visible_row = MAX(0, (u32) floorf(ImGui::GetScrollY() / row_height));
        u32 last_visible_row = MIN(flattened_sorted_folder_task_tree.length, (u32) ceilf((ImGui::GetScrollY() + content_height) / row_height));

        u32 first_top_level_task_row = first_visible_row;

        while (first_top_level_task_row > 0 && flattened_sorted_folder_task_tree[first_top_level_task_row].nesting_level) {
            first_top_level_task_row--;
        }

        for (u32 row = first_top_level_task_row; row < last_visible_row; row++) {
            Flattened_Folder_Task* flattened_task = &flattened_sorted_folder_task_tree[row];

            bool is_expanded = flattened_task->sorted_task->is_expanded;
            bool needs_to_be_sorted = flattened_task->needs_sub_task_sort;
            bool has_more_than_one_visible_task = flattened_task->num_visible_sub_tasks > 1;

            if (is_expanded && needs_to_be_sorted && has_more_than_one_visible_task) {
                sort_sub_tasks_of_task(flattened_task->sorted_task);
                rebuild_flattened_task_subtree(flattened_task);

                flattened_task->needs_sub_task_sort = false;
            }
        }

        for (u32 column = 0; column < paint_context.total_columns; column++) {
            float column_width = get_column_width(paint_context, column);

            for (u32 row = first_visible_row; row < last_visible_row; row++) {
                Flattened_Folder_Task* flattened_task = &flattened_sorted_folder_task_tree[row];

                float row_top_y = row_height * (row + 1);

                ImVec2 top_left(column_left_x, row_top_y);
                top_left += window_top_left;

                ImGui::PushID(flattened_task);

                draw_table_cell_for_task(paint_context, column, column_width, flattened_task, top_left);

                ImGui::PopID();
            }

            ImVec2 column_top_left_absolute = window_top_left + ImVec2(column_left_x, 0) + ImVec2(0, ImGui::GetScrollY());
            ImVec2 column_bottom_left_absolute = window_top_left + ImVec2(column_left_x, content_height) + ImVec2(0, ImGui::GetScrollY());

            draw_list->AddRectFilled(column_top_left_absolute + ImVec2(column_width, 0), window_bottom_right_no_scroll, IM_COL32_WHITE);
            draw_list->AddLine(column_top_left_absolute, column_bottom_left_absolute, grid_color, 1.25f);

            column_left_x += column_width;
        }

        for (u32 row = first_visible_row; row < last_visible_row; row++) {
            float row_line_y = row_height * (row + 1);

            draw_list->AddLine(window_top_left + ImVec2(0, row_line_y), window_top_left + ImVec2(column_left_x, row_line_y), grid_color, 1.25f);
        }

        ImGui::PopFont();

        draw_table_header(paint_context, window_top_left);

        // Scrollbar
        ImGui::Dummy(ImVec2(column_left_x, flattened_sorted_folder_task_tree.length * row_height));
        ImGui::EndChild();

        if (queue_flattened_tree_rebuild) {
            queue_flattened_tree_rebuild = false;
            rebuild_flattened_task_tree();
        }

        u32
                loading_end_time = MAX(finished_loading_folder_contents_at, finished_loading_statuses_at);
                loading_end_time = MAX(finished_loading_folder_header_at, loading_end_time);
                loading_end_time = MAX(finished_loading_users_at, loading_end_time);
                loading_end_time = MAX(finished_loading_statuses_at, loading_end_time);
                loading_end_time = MAX(started_showing_main_ui_at, loading_end_time);

        float alpha = lerp(loading_end_time, tick, 1.0f, 8);

        ImGui::FadeInOverlay(alpha);
    } else {
        u32
                loading_start_time = MIN(started_loading_folder_contents_at, started_loading_statuses_at);
                loading_start_time = MIN(started_loading_users_at, loading_start_time);
                loading_start_time = MIN(started_loading_statuses_at, loading_start_time);

        draw_window_loading_indicator();
    }

    ImGui::EndChildFrame();
}

static void process_folder_contents_data_object(char* json, jsmntok_t*& token) {
    jsmntok_t* object_token = token++;

    assert(object_token->type == JSMN_OBJECT);

    Folder_Task* folder_task = &folder_tasks[folder_tasks.length];
    folder_task->num_parent_task_ids = 0;
    folder_task->num_parent_folder_ids = 0;
    folder_task->num_custom_field_values = 0;
    folder_task->num_assignees = 0;

    Sorted_Folder_Task* sorted_folder_task = &sorted_folder_tasks[folder_tasks.length];
    sorted_folder_task->num_sub_tasks = 0;
    sorted_folder_task->source_task = folder_task;
    sorted_folder_task->is_expanded = false;

    folder_tasks.length++;

    for (u32 propety_index = 0; propety_index < object_token->size; propety_index++, token++) {
        jsmntok_t* property_token = token++;

        assert(property_token->type == JSMN_STRING);

        jsmntok_t* next_token = token;

        if (json_string_equals(json, property_token, "title")) {
            json_token_to_string(json, next_token, folder_task->title);
        } else if (json_string_equals(json, property_token, "id")) {
            json_token_to_right_part_of_id16(json, next_token, folder_task->id);
        } else if (json_string_equals(json, property_token, "customStatusId")) {
            json_token_to_right_part_of_id16(json, next_token, folder_task->custom_status_id);

            folder_task->custom_status_id_hash = hash_id(folder_task->custom_status_id);
        } else if (json_string_equals(json, property_token, "responsibleIds")) {
            assert(next_token->type == JSMN_ARRAY);

            token++;

            if (next_token->size > 0) {
                folder_task->assignees = lazy_array_reserve_n_values_relative_pointer(assignee_ids, next_token->size);
            }

            for (u32 field_index = 0; field_index < next_token->size; field_index++, token++) {
                json_token_to_id8(json, token, folder_task->assignees[folder_task->num_assignees++]);
            }

            token--;
        } else if (json_string_equals(json, property_token, "parentIds")) {
            assert(next_token->type == JSMN_ARRAY);

            token++;

            if (next_token->size > 0) {
                folder_task->parent_folder_ids = lazy_array_reserve_n_values_relative_pointer(parent_task_ids, next_token->size);
            }

            for (u32 field_index = 0; field_index < next_token->size; field_index++, token++) {
                json_token_to_right_part_of_id16(json, token, folder_task->parent_folder_ids[folder_task->num_parent_folder_ids++]);
            }

            token--;
        } else if (json_string_equals(json, property_token, "superTaskIds")) {
            assert(next_token->type == JSMN_ARRAY);

            token++;

            if (next_token->size > 0) {
                folder_task->parent_task_ids = lazy_array_reserve_n_values_relative_pointer(parent_task_ids, next_token->size);
            }

            for (u32 field_index = 0; field_index < next_token->size; field_index++, token++) {
                json_token_to_right_part_of_id16(json, token, folder_task->parent_task_ids[folder_task->num_parent_task_ids++]);
            }

            token--;
        } else if (json_string_equals(json, property_token, "customFields")) {
            assert(next_token->type == JSMN_ARRAY);

            token++;

            if (next_token->size > 0) {
                folder_task->custom_field_values = lazy_array_reserve_n_values_relative_pointer(custom_field_values, next_token->size);
            }

            for (u32 field_index = 0; field_index < next_token->size; field_index++) {
                Custom_Field_Value* value = &folder_task->custom_field_values[folder_task->num_custom_field_values++];

                // TODO a dependency on task_view is not really good, should we move the code somewhere else?
                process_task_custom_field_value(value, json, token);
            }

            token--;
        } else {
            eat_json(token);
            token--;
        }
    }

    sorted_folder_task->id = folder_task->id;
    sorted_folder_task->id_hash = hash_id(folder_task->id);

    id_hash_map_put(&id_to_sorted_folder_task, sorted_folder_task, folder_task->id, sorted_folder_task->id_hash);
}

void process_folder_header_data(char* json, u32 data_size, jsmntok_t*& token) {
    // We only request singular folders now
    assert(data_size == 1);
    assert(token->type == JSMN_OBJECT);

    jsmntok_t* object_token = token++;

    for (u32 propety_index = 0; propety_index < object_token->size; propety_index++, token++) {
        jsmntok_t* property_token = token++;

        assert(property_token->type == JSMN_STRING);

        jsmntok_t* next_token = token;

        if (json_string_equals(json, property_token, "title")) {
            json_token_to_string(json, next_token, current_folder.name);
        } else if (json_string_equals(json, property_token, "customColumnIds")) {
            current_folder.custom_columns = (Custom_Field_Id*) REALLOC(current_folder.custom_columns, sizeof(Custom_Field_Id) * next_token->size);
            current_folder.num_custom_columns = 0;

            for (u32 array_index = 0; array_index < next_token->size; array_index++) {
                jsmntok_t* id_token = ++token;

                assert(id_token->type == JSMN_STRING);

                json_token_to_right_part_of_id16(json, id_token, current_folder.custom_columns[current_folder.num_custom_columns++]);
            }
        } else {
            eat_json(token);
            token--;
        }
    }
}

static void associate_parent_tasks_with_sub_tasks(Folder_Id top_parent_id) {
    u32 total_sub_tasks = 0;

    // Step 0: determine and populate top level tasks
    for (u32 task_index = 0; task_index < folder_tasks.length; task_index++) {
        Sorted_Folder_Task* folder_task = &sorted_folder_tasks[task_index];
        Folder_Task* source_task = folder_task->source_task;

        for (u32 id_index = 0; id_index < source_task->num_parent_folder_ids; id_index++) {
            Folder_Id parent_id = source_task->parent_folder_ids[id_index];

            if (parent_id == top_parent_id) {
                Sorted_Folder_Task** pointer_to_task = lazy_array_reserve_n_values(top_level_tasks, 1);
                *pointer_to_task = folder_task;
            }
        }
    }

    // Step 1: count sub tasks for each parent, while also deciding which parents should go into the root
    for (u32 task_index = 0; task_index < folder_tasks.length; task_index++) {
        Sorted_Folder_Task* folder_task = &sorted_folder_tasks[task_index];
        Folder_Task* source_task = folder_task->source_task;

        for (u32 id_index = 0; id_index < source_task->num_parent_task_ids; id_index++) {
            Task_Id parent_id = source_task->parent_task_ids[id_index];
            Sorted_Folder_Task* parent_or_null = id_hash_map_get(&id_to_sorted_folder_task, parent_id, hash_id(parent_id));

            if (parent_or_null) {
                parent_or_null->num_sub_tasks++;
                total_sub_tasks++;
            }
        }
    }

    // Step 2: allocate space for sub tasks
    sub_tasks = (Sorted_Folder_Task**) REALLOC(sub_tasks, total_sub_tasks * sizeof(Sorted_Folder_Task*));
    total_sub_tasks = 0;

    for (u32 task_index = 0; task_index < folder_tasks.length; task_index++) {
        Sorted_Folder_Task* folder_task = &sorted_folder_tasks[task_index];

        if (folder_task->num_sub_tasks) {
            folder_task->sub_tasks = sub_tasks + total_sub_tasks;

            total_sub_tasks += folder_task->num_sub_tasks;

            folder_task->num_sub_tasks = 0;
        }
    }

    // Step 3: fill sub tasks
    for (u32 task_index = 0; task_index < folder_tasks.length; task_index++) {
        Sorted_Folder_Task* folder_task = &sorted_folder_tasks[task_index];
        Folder_Task* source_task = folder_task->source_task;

        for (u32 id_index = 0; id_index < source_task->num_parent_task_ids; id_index++) {
            Task_Id parent_id = source_task->parent_task_ids[id_index];

            Sorted_Folder_Task* parent_or_null = id_hash_map_get(&id_to_sorted_folder_task, parent_id, hash_id(parent_id));

            if (parent_or_null) {
                parent_or_null->sub_tasks[parent_or_null->num_sub_tasks++] = folder_task;
            }
        }
    }
}

void process_folder_contents_data(char* json, u32 data_size, jsmntok_t*& token) {
    if (id_to_sorted_folder_task.table) {
        id_hash_map_destroy(&id_to_sorted_folder_task);
    }

    id_hash_map_init(&id_to_sorted_folder_task);

    if (folder_tasks.length < data_size) {
        folder_tasks.data = (Folder_Task*) REALLOC(folder_tasks.data, sizeof(Folder_Task) * data_size);
        sorted_folder_tasks = (Sorted_Folder_Task*) REALLOC(sorted_folder_tasks, sizeof(Sorted_Folder_Task) * data_size);
        flattened_sorted_folder_task_tree.data = (Flattened_Folder_Task*) REALLOC(flattened_sorted_folder_task_tree.data, sizeof(Flattened_Folder_Task) * data_size);
    }

    folder_tasks.length = 0;

    lazy_array_soft_reset(custom_field_values);
    lazy_array_soft_reset(parent_task_ids);
    lazy_array_soft_reset(top_level_tasks);

    for (u32 array_index = 0; array_index < data_size; array_index++) {
        process_folder_contents_data_object(json, token);
    }

    associate_parent_tasks_with_sub_tasks(current_folder.id);

    has_been_sorted_after_loading = false;
}

void set_current_folder_id(Folder_Id id) {
    current_folder.id = id;
}

void process_current_folder_as_logical() {
    current_folder.num_custom_columns = 0;
}