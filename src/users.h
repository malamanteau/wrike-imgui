#include <jsmn.h>
#include "common.h"
#include "temporary_storage.h"
#include "main.h"

#pragma once

struct User {
    User_Id id;
    String first_name;
    String last_name;
    String avatar_url;

    Request_Id avatar_request_id;
    Memory_Image avatar{};
    u32 avatar_loaded_at;
};

extern Array<User> users;
extern Array<User> suggested_users;

extern User* this_user;

void process_users_data(char* json, u32 data_size, jsmntok_t*& token);
void process_suggested_users_data(char* json, u32 data_size, jsmntok_t*&token);

User* find_user_by_id(User_Id id, u32 id_hash = 0);
User* find_user_by_avatar_request_id(Request_Id avatar_request_id);

bool check_and_request_user_avatar_if_necessary(User* user);

inline String full_user_name_to_temporary_string(User* user) {
    // This function used tprintf("%.*s %.*s", ...) earlier, but turns out snprintf is ridiculously slow
    String result;
    result.length = user->first_name.length + user->last_name.length + 1;
    result.start = (char*) talloc(result.length);

    memcpy(result.start, user->first_name.start, user->first_name.length);
    memcpy(result.start + user->first_name.length + 1, user->last_name.start, user->last_name.length);

    result.start[user->first_name.length] = ' ';

    return result;
}