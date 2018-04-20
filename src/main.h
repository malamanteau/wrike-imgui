#pragma once

#include "common.h"
#include "rich_text.h"

bool init();

extern "C"
void loop();

struct Task {
    String title;
    String permalink;
    Rich_Text_String* description = NULL;
    u32 description_strings = 0;
    String description_text{};

    Id8* assignees = NULL;
    u32 num_assignees = 0;
};

struct User {
    Id8 id;
    String firstName;
    String lastName;
};

extern User* users;
extern u32 users_count;

typedef signed long Request_Id;

// TODO make this a define?
extern const Request_Id NO_REQUEST;

// TODO we could actually have a struct which encompasses Request_Id, started_at and finished_at
extern Request_Id folder_tree_request;
extern Request_Id folder_header_request;
extern Request_Id folder_contents_request;
extern Request_Id task_request;
extern Request_Id contacts_request;
extern Request_Id accounts_request;
extern Request_Id workflows_request;

extern u32 started_loading_folder_contents_at;
extern u32 finished_loading_folder_contents_at;
extern u32 finished_loading_folder_header_at;

extern u32 started_loading_task_at;
extern u32 finished_loading_task_at;

extern u32 started_loading_statuses_at;
extern u32 finished_loading_statuses_at;

extern bool had_last_selected_folder_so_doesnt_need_to_load_the_root_folder;

extern u32 tick;

extern Task current_task;
extern Id16 selected_folder_task_id;

void request_task(Id16& task_id);

// TODO move this into imgui extension file?
namespace ImGui {
    void LoadingIndicator(u32 started_showing_at);
    void Image(Memory_Image& image);
}