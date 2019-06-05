//=============================================================================
// >>> 4Coder vim custom base <<<
// author: chr <chr@chronal.net>
//
#include "4coder_default_include.cpp"
//
// 4vim is a prototype vim-binding implementation for 4coder. It is intended to
// serve as a reusable system that overlays vim functionality on top of 4coder
// and allows your custom to operate more like vim.
//
// To use this, you *must* do the following:
// 
// 1. Define and forward 4coder hooks:
//     - In your start hook, call vim_hook_init_func(app)
//     - In your open file hook, call vim_hook_open_file_func(app, buffer_id)
//     - In your new file hook, call vim_hook_new_file_func(app, buffer_id)
//     - In your get bindings hook, call vim_get_bindings(context)
//
// 2. Define the following functions:
//
void on_enter_normal_mode(struct Application_Links* app);
void on_enter_insert_mode(struct Application_Links* app);
void on_enter_replace_mode(struct Application_Links* app);
void on_enter_visual_mode(struct Application_Links* app);
//
//    The definitions may be empty, but they need to exist or the linker will
//    complain. TODO(chr). They allow you to hook into vim binding behavior in
//    your custom, for example to change the color scheme when certain actions
//    occur.
//
// That's it! See the included 4coder_chronal.cpp for examples of adding key
// bindings, mode change hooks, status bar commands, and other customizations.
//
// If you have questions or feature requests, feel free to reach out in the
// GitHub issues at https://github.com/chr-1x/4vim.
//
// Personal TODOs:
//  - Freshly opened files aren't in normal mode?
//  - * search should delimit with word boundaries
//  - dw at end of line shouldn't delete newline
//  - s (equivalent to cl)
//  - S (delete contents of line and go to insert mode at appropriate indentation)
//    - equivalent to cc
//  - Range reformatting gq
//    - v1: comment wrapping
//  - Autocomment on new line
//  - Support some basic vim variables via set
//  - Visual block mode
//  - Code folding?
//
//=============================================================================

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

//=============================================================================
// > Types <
// The vim custom uses these to keep track of its state and overlay some
// functionality on top of the built in 4coder behavior.
//=============================================================================

enum Vim_Maps {
    mapid_unbound = mapid_global,
    mapid_movements = 80000,
    mapid_normal,
    mapid_insert,
    mapid_replace,
    mapid_visual,

    // There are a bunch of different chord "starters" that result in keys having
    // different behaviors. 

    mapid_chord_replace_single,
    mapid_chord_yank,
    mapid_chord_delete,
    mapid_chord_indent_left,
    mapid_chord_indent_right,
    mapid_chord_format,
    mapid_chord_mark,
    mapid_chord_g,
    mapid_chord_window,
    mapid_chord_choose_register,
    mapid_chord_move_find,
    mapid_chord_move_til,
    mapid_chord_move_rfind,
    mapid_chord_move_rtil,
    mapid_chord_move_in,
};

enum Vim_Mode {
    mode_normal,
    mode_insert,
    mode_replace,
    mode_visual,
    mode_visual_line,
};

enum Pending_Action {
    vimaction_none,

    vimaction_delete_range,
    vimaction_change_range,
    vimaction_yank_range,
    vimaction_format_range,
    vimaction_indent_left_range,
    vimaction_indent_right_range,
};

struct Vim_Register {
    String text;
    bool is_line;
};

enum Register_Id {
    reg_unnamed = 0,
    reg_system_clipboard,
    reg_a, reg_b, reg_c, reg_d, reg_e, reg_f, reg_g, reg_h, reg_i, 
    reg_j, reg_k, reg_l, reg_m, reg_n, reg_o, reg_p, reg_q, reg_r, 
    reg_s, reg_t, reg_u, reg_v, reg_w, reg_x, reg_y, reg_z,
    reg_1, reg_2, reg_3, reg_4, reg_5, reg_6, reg_7, reg_8, reg_9, reg_0 
};

Register_Id regid_from_char(Key_Code C) {
    if ('a' <= C && C <= 'z') {
        return (Register_Id)(reg_a + (C - 'a'));
    }

    if ('A' <= C && C <= 'Z') {
        return (Register_Id)(reg_a + (C - 'A'));
    }

    if ('1' <= C && C <= '9') {
        return (Register_Id)(reg_1 + (C - '1'));
    }
    if (C == '0') { return reg_0; }

    if (C == '*') { return reg_system_clipboard; }

    return reg_unnamed;
}

enum Search_Direction {
    search_backward = -1,
    search_forward = 1,
};

struct Search_Context {
    Search_Direction direction;
    String text;
    char text_buffer[100];
};

struct Vim_Query_Bar {
    bool exists;
    Query_Bar bar;
    char contents[50];
    int contents_len;
};

struct Vim_State {
    // 37 clipboard registers:
    //  - 1 unnamed
    //  - 1 sysclipboard
    //  - 26 letters
    //  - 10 numbers
    Vim_Register registers[38];

    // 36 Mark offsets:
    //  - 26 letters
    //  - 10 numbers
    int marks[36];

    // The *current* vim mode. If a chord or action is pending, this will dictate
    // what mode you return to once the action is completed.
    Vim_Mode mode;
    // A pending action. Used to keep track of intended edits while in the middle
    // of chords.
    Pending_Action action;
    // The current register. Union for convenience (and to make it clear that
    // only one of these things can be happening at once).
    union {
        Register_Id action_register;
        Register_Id yank_register;
        Register_Id paste_register;
    };
    // The state of the selection:
    //  - start is where the selection was started
    //  - end is where the cursor is during the selection
    Range selection_cursor;
    // The effective selection area:
    //  - normalized range which should be used for cut/copy
    //    operations
    Range selection_range;

    // TODO(chr): Actually there needs to be one of these per file!
    // Until I can use the GUI customization to make my own, anyway.
    Vim_Query_Bar chord_bar;

    Search_Context last_search;
};

#define VIM_COMMAND_FUNC_SIG(n) void n(struct Application_Links *app,         \
                                       const String command,                  \
                                       const String argstr,                   \
                                       bool force)
typedef VIM_COMMAND_FUNC_SIG(Vim_Command_Func);

struct Vim_Command_Defn {
    String command;
    Vim_Command_Func* func;
};

//=============================================================================
// > Global Variables <
// I hope I can use 4coder's API to avoid having these eventually.
//=============================================================================

static Vim_State state = {};

// TODO(chr): Make these be dynamic and be a hashtable
static Vim_Command_Defn defined_commands[512];
static int defined_command_count = 0;

//=============================================================================
// > Helpers <                                                         @helpers
// Some miscellaneous helper structs and functions.
//=============================================================================

// Defer:                                                               @defer
// Run some code when the scope exits, regardless of how it exited.
// Wah wah C++. Why can't I use a lambda without including this massive header?
#include <functional>

using defer_func = std::function<void()>;
struct _Defer {
    defer_func the_func;
    _Defer(defer_func func) : the_func(func) {}
    ~_Defer() { the_func(); }
};
#define defer(s) _Defer defer##__LINE__([&] { s; })

// Iterate over views:                                               @for_views
#define for_views(view, app)                                                  \
    for (View_Summary view = get_view_first(app, AccessAll);                  \
         view.exists;                                                         \
         get_view_next(app, &view, AccessAll))

// Linux specific magic to deal with ~ expansion
#if defined(IS_LINUX)
#include <pwd.h>
#include <unistd.h>
#endif

static int32_t get_user_home_dir(char* out, int32_t out_mem_size) {
#if defined(IS_LINUX)
    uid_t uid = getuid();
    struct passwd *pw = getpwuid(uid);
    int32_t homedir_len = strlen(pw->pw_dir);
    if (homedir_len <= out_mem_size && out != nullptr) {
        strncpy(out, pw->pw_dir, homedir_len);
    }
    return homedir_len;
#else
    return -1;
#endif
}

namespace {

// Forward declare these for ease of use since they call between each other
static void enter_normal_mode(struct Application_Links *app, int buffer_id);
static void enter_insert_mode(struct Application_Links *app, int buffer_id);
static void update_visual_range(struct Application_Links* app, int end_new);
static void update_visual_line_range(struct Application_Links* app,
                                     int end_new);
static void end_visual_selection(struct Application_Links* app);
static void copy_into_register(struct Application_Links* app,
                               Buffer_Summary* buffer, Range range,
                               Vim_Register* target_register);
static bool active_view_to_line(struct Application_Links* app, int line);
static int get_line_start(struct Application_Links* app, int cursor = -1);
static int get_cursor_pos(struct Application_Links* app);
static char get_cursor_char(struct Application_Links* app, int offset = 0);
static void push_to_chord_bar(struct Application_Links* app, const String str);
static void end_chord_bar(struct Application_Links* app);
static void clear_register_selection();
static void vim_exec_action(struct Application_Links* app, Range range,
                            bool is_line = false);

static bool directory_cd_expand_user(
    struct Application_Links* app,
    char* dir_str,
    int32_t* dir_len,
    int32_t dir_capacity,
    char* rel_path,
    int32_t rel_len) {
    if (rel_len > 0 && rel_path[0] == '~') {
        int32_t home_dir_len = get_user_home_dir(nullptr, 0);
        int32_t expanded_len = home_dir_len + (rel_len - 1);
        if (expanded_len > dir_capacity) { return false; }
        get_user_home_dir(dir_str, dir_capacity);
        String dir = make_string_cap(dir_str, home_dir_len, dir_capacity);
        append_ss(&dir, make_string(rel_path + 1, rel_len - 1));
        *dir_len = dir.size;
        return true;
    } else {
        return directory_cd(app, dir_str, dir_len, dir_capacity, rel_path, rel_len);
    }
}

static void enter_insert_mode(struct Application_Links *app, int buffer_id) {
    unsigned int access = AccessAll;
    Buffer_Summary buffer;
    
    if (state.mode == mode_visual ||
        state.mode == mode_visual_line) {
        end_visual_selection(app);
    }

    state.action = vimaction_none;
    state.mode = mode_insert;
    end_chord_bar(app);

    buffer = get_buffer(app, buffer_id, access);
    buffer_set_setting(app, &buffer, BufferSetting_MapID, mapid_insert);

    on_enter_insert_mode(app);
}

static void copy_into_register(struct Application_Links* app,
                               Buffer_Summary* buffer, Range range,
                               Vim_Register* target_register) {
    free(target_register->text.str);
    target_register->text = make_string((char*)malloc(range.end - range.start), range.end - range.start);
    buffer_read_range(app, buffer, range.start, range.end, target_register->text.str);
    if (target_register == &state.registers[reg_system_clipboard]) {
        clipboard_post(app, 0, target_register->text.str, target_register->text.size);
    }
}

static void paste_from_register(struct Application_Links* app,
                                Buffer_Summary* buffer, int paste_pos,
                                Vim_Register* reg) {
    if (reg == &state.registers[reg_system_clipboard]) {
        free(reg->text.str);
        int clipboard_text_size = clipboard_index(app, 0, 0, NULL, 0);
        reg->text = make_string((char*)malloc(clipboard_text_size), clipboard_text_size);
        clipboard_index(app, 0, 0, reg->text.str, reg->text.size);
    }
    buffer_replace_range(app, buffer, paste_pos, paste_pos,
                         reg->text.str, reg->text.size);
}

static void buffer_search(struct Application_Links* app, String word,
                          View_Summary view, Search_Direction direction) {
    const auto& buffer_seek_func =
        (direction == search_forward ? buffer_seek_string_forward :
         buffer_seek_string_backward);

    Buffer_Summary buffer = get_buffer(app, view.buffer_id, AccessAll);
    int start_pos = view.cursor.pos;
    int new_pos = start_pos;

    buffer_seek_func(app, &buffer, view.cursor.pos + direction, 0, word.str,
                     word.size, &new_pos);
    if (new_pos < buffer.size && new_pos >= 0) {
        view_set_cursor(app, &view, seek_pos(new_pos), true);
    } else {
        int wrap = (direction == search_forward ? 0 : buffer.size - 1);
        buffer_seek_func(app, &buffer, wrap, 0, word.str, word.size, &new_pos);
        if (new_pos < buffer.size && new_pos >= 0) {
            view_set_cursor(app, &view, seek_pos(new_pos), true);
        }
    }
    refresh_view(app, &view);
    int actual_new_cursor_pos = view.cursor.pos;
    // Update last_search
    state.last_search.direction = direction;
    state.last_search.text = make_fixed_width_string(
        state.last_search.text_buffer);
    append_checked_ss(&state.last_search.text, word);
    // Do the motion
    vim_exec_action(app, make_range(start_pos, actual_new_cursor_pos), false);
}

static bool active_view_to_line(struct Application_Links* app, int line) {
    View_Summary view = get_active_view(app, AccessProtected);
    if (!view.exists) return false;
    if (!view_set_cursor(app, &view, seek_line_char(line, 0), false)) {
        return false;
    }
    return true;
}

static int get_current_view_buffer_id(struct Application_Links* app,
                                      int access) {
    View_Summary view = get_active_view(app, access);
    return view.buffer_id;
}

static void set_current_keymap(struct Application_Links* app, int map) {
    View_Summary view = get_active_view(app, AccessAll);
    Buffer_Summary buffer = get_buffer(app, view.buffer_id, AccessAll);
    if (!buffer.exists) { return; }
    buffer_set_setting(app, &buffer, BufferSetting_MapID, map);
}

static char get_cursor_char(struct Application_Links* app, int offset) {
    View_Summary view = get_active_view(app, AccessOpen);
    Buffer_Summary buffer = get_buffer(app, view.buffer_id, AccessOpen);
    char read; 
    int res = buffer_read_range(app, &buffer, view.cursor.pos + offset,
                                view.cursor.pos + offset + 1, &read);
    if (res) { return read; }
    else { return 0; }
}

static int get_cursor_pos(struct Application_Links* app) {
    View_Summary view = get_active_view(app, AccessAll);
    return view.cursor.pos;
}

static int get_line_start(struct Application_Links* app, int cursor) {
    unsigned int access = AccessAll;
    View_Summary view = get_active_view(app, access);
    Buffer_Summary buffer = get_buffer(app, view.buffer_id, access);
    if (cursor == -1) {
        cursor = view.cursor.pos;
    }
    
    int new_pos = seek_line_beginning(app, &buffer, cursor);
    return new_pos;
}

static int get_line_end(struct Application_Links* app, int cursor = -1) {
    unsigned int access = AccessAll;
    View_Summary view = get_active_view(app, access);
    Buffer_Summary buffer = get_buffer(app, view.buffer_id, access);
    if (cursor == -1) {
        cursor = view.cursor.pos;
    }
    
    int new_pos = seek_line_end(app, &buffer, cursor);
    return new_pos;
}

static void update_visual_range(struct Application_Links* app, int end_new) {
    View_Summary view;
    
    unsigned int access = AccessOpen;
    view = get_active_view(app, access);

    state.selection_cursor.end = end_new;
    Range normalized = make_range(state.selection_cursor.start, state.selection_cursor.end);
    state.selection_range = make_range(normalized.start, normalized.end + 1);
}

static void update_visual_line_range(struct Application_Links* app, int end_new) {
    View_Summary view;
    
    unsigned int access = AccessOpen;
    view = get_active_view(app, access);

    state.selection_cursor.end = end_new;
    Range normalized = make_range(state.selection_cursor.start, state.selection_cursor.end);
    state.selection_range = make_range(get_line_start(app, normalized.start), 
                                       get_line_end(app, normalized.end) + 1);
}

static void end_visual_selection(struct Application_Links* app) {
    View_Summary view;
    
    unsigned int access = AccessOpen;
    view = get_active_view(app, access);

    state.selection_range.start = state.selection_range.end = -1;
    state.selection_cursor.start = state.selection_cursor.end = -1;
}

static int push_to_string(char* str, size_t str_len, size_t str_max,
                          char* dest, size_t dest_len) {
    int i = 0;
    for (i; i < dest_len && i < (str_max - str_len); ++i)
    {
        str[str_len + i] = dest[i];
    }
    return i + (int)str_len;
}

static void push_to_chord_bar(struct Application_Links* app, const String str) {
    if (!state.chord_bar.exists) {
        if (start_query_bar(app, &state.chord_bar.bar, 0) == 0) return;
        state.chord_bar.contents_len = 0;
        memset(state.chord_bar.contents, '\0',
               ArrayCount(state.chord_bar.contents));
        state.chord_bar.exists = true;
    }
    state.chord_bar.contents_len = push_to_string(
        state.chord_bar.contents, state.chord_bar.contents_len,
        ArrayCount(state.chord_bar.contents), str.str, str.size);
    state.chord_bar.bar.string = make_string(
        state.chord_bar.contents, state.chord_bar.contents_len,
        ArrayCount(state.chord_bar.contents));
}

static void end_chord_bar(struct Application_Links* app) {
    if (state.chord_bar.exists) {
        end_query_bar(app, &state.chord_bar.bar, 0);
        state.chord_bar.contents_len = 0;
        memset(state.chord_bar.contents, '\0',
               ArrayCount(state.chord_bar.contents));
        state.chord_bar.exists = false;
    }
}

static void clear_register_selection() {
    state.yank_register = state.paste_register = reg_unnamed;
}

static void vim_exec_action(struct Application_Links* app, Range range,
                            bool is_line) {
    View_Summary view = get_active_view(app, AccessAll);
    Buffer_Summary buffer = get_buffer(app, view.buffer_id, AccessAll);

    switch (state.action) {
        case vimaction_delete_range: 
        case vimaction_change_range: {
            Vim_Register* target_register = state.registers + state.yank_register;
            target_register->is_line = is_line;
 
            copy_into_register(app, &buffer, range, state.registers + state.yank_register);
            
            buffer_replace_range(app, &buffer, range.start, range.end, "", 0);

            if (state.action == vimaction_change_range) {
                enter_insert_mode(app, buffer.buffer_id);
            }
        } break;

        case vimaction_yank_range: {
            Vim_Register* target_register = state.registers + state.yank_register;
            target_register->is_line = is_line;

            copy_into_register(app, &buffer, range, target_register);
        } break;

        case vimaction_indent_left_range:  // TODO(chr)
        case vimaction_indent_right_range:
        case vimaction_format_range: {
            // TODO(chr) tab width as a user variable
            buffer_auto_indent(app, &buffer, range.start, range.end - 1, 4, 0);
        } break;
    }

    switch (state.mode) {
        case mode_normal: {
            enter_normal_mode(app, buffer.buffer_id);
        } break;

        case mode_visual: {
            update_visual_range(app, view.cursor.pos);
            set_current_keymap(app, mapid_visual);
        } break;

        case mode_visual_line: {
            update_visual_line_range(app, view.cursor.pos);
            set_current_keymap(app, mapid_visual);
        } break;
    }
}

static int buffer_seek_next_word(Application_Links* app, Buffer_Summary* buffer,
                                 int pos) {
    char chunk[1024];
    int chunk_size = sizeof(chunk);
    Stream_Chunk stream = {};
    
    if (init_stream_chunk(&stream, app, buffer, pos, chunk, chunk_size)) {
        char cursorch = stream.data[pos];
        char nextch = cursorch; 
        int still_looping = true;
        bool inter_whitespace = false;
        do {
            for (; pos < stream.end; ++pos) {
                // Three kinds of characters:
                //  - word characters, first of a row results in a stop
                //  - symbol characters, first of a row results in a stop
                //  - whitespace characters, always skip
                //  The distinction between the first two is only needed
                //   because word and symbol characters do not form a "row"
                //   when intermixed.
                nextch = stream.data[pos];
                int is_whitespace = char_is_whitespace(nextch);
                int is_alphanum = char_is_alpha_numeric(nextch);
                int is_symbol = !is_whitespace && !is_alphanum;

                if (char_is_whitespace(cursorch)) {
                    if (!is_whitespace) {
                        return pos;
                    }
                }
                else if (char_is_alpha_numeric(cursorch)) {
                    if (is_whitespace) {
                        inter_whitespace = true;
                    }
                    else if (is_symbol ||
                             (is_alphanum && inter_whitespace)) {
                        return pos;
                    }
                }
                else {
                    if (is_whitespace) {
                        inter_whitespace = true;
                    }
                    if (is_alphanum ||
                        (is_symbol && inter_whitespace)) {
                        return pos;
                    }
                }
            }
            still_looping = forward_stream_chunk(&stream);
        } while (still_looping);

        if (pos > buffer->size) {
            pos = buffer->size;
        }
    }

    return pos;
}

static int buffer_seek_nonalphanumeric_right(Application_Links* app,
                                             Buffer_Summary* buffer, int pos) {
    char chunk[1024];
    int chunk_size = sizeof(chunk);
    Stream_Chunk stream = {};
    
    if (init_stream_chunk(&stream, app, buffer, pos, chunk, chunk_size)) {
        char cursorch = stream.data[pos];
        char nextch = cursorch; 
        int still_looping = true;
        do {
            for (; pos < stream.end; ++pos) {
                nextch = stream.data[pos];
                if (!char_is_alpha_numeric(nextch)) {
                    return pos;
                }
            }
            still_looping = forward_stream_chunk(&stream);
        } while (still_looping);

        if (pos > buffer->size) {
            pos = buffer->size;
        }
    }

    return pos;
}

static int buffer_seek_nonalphanumeric_left(Application_Links* app,
                                            Buffer_Summary* buffer, int pos) {
    char chunk[1024];
    int chunk_size = sizeof(chunk);
    Stream_Chunk stream = {};
    
    if (init_stream_chunk(&stream, app, buffer, pos, chunk, chunk_size)) {
        char cursorch = stream.data[pos];
        char nextch = cursorch; 
        int still_looping = true;
        do {
            for (; pos >= stream.start; --pos) {
                nextch = stream.data[pos];
                if (!char_is_alpha_numeric(nextch)) {
                    return pos;
                }
            }
            still_looping = backward_stream_chunk(&stream);
        } while (still_looping);

        if (pos > buffer->size) {
            pos = buffer->size;
        }
    }

    return pos;
}

static Range get_word_under_cursor(struct Application_Links* app,
                                   Buffer_Summary* buffer,
                                   View_Summary* view) {
    int pos = view->cursor.pos;
    int start = buffer_seek_nonalphanumeric_left(app, buffer, pos) + 1;
    int end = buffer_seek_nonalphanumeric_right(app, buffer, pos);
    return make_range(start, end);
}

static void enter_normal_mode(struct Application_Links *app, int buffer_id) {
    if (state.mode == mode_insert || state.mode == mode_replace) {
        move_left(app);   
    }
    if (state.mode == mode_visual || state.mode == mode_visual_line) {
        end_visual_selection(app);
    }
    state.action = vimaction_none;
    end_chord_bar(app);
    Buffer_Summary buffer = get_buffer(app, buffer_id, AccessAll);
    buffer_set_setting(app, &buffer, BufferSetting_MapID, mapid_normal);
    if (state.mode != mode_normal) {
        state.mode = mode_normal;
        on_enter_normal_mode(app);
    }
}

static void buffer_query_search(struct Application_Links* app,
                                Search_Direction direction) {
    View_Summary view = get_active_view(app, AccessAll);
    Buffer_Summary buffer = get_buffer(app, view.buffer_id, AccessAll);
    if (!buffer.exists) return;
    // Start the search query bar
    Query_Bar bar;
    if (start_query_bar(app, &bar, 0) == 0) return;
    defer(end_query_bar(app, &bar, 0));
    // Bar string
    char bar_string_space[256];
    bar.string = make_fixed_width_string(bar_string_space);
    bar.prompt = make_lit_string(direction == search_forward ? "/" : "?");
    // Handle the query bar
    User_Input in;
    while (true) {
        in = get_user_input(app, EventOnAnyKey, EventOnEsc);
        if (in.abort) break;
        if (in.key.keycode == '\n'){
            break;
        }
        else if (in.key.keycode == '\t') {
            // Ignore it
        }
        else if (in.key.character && key_is_unmodified(&in.key)){
            append(&bar.string, (char)in.key.character);
        }
        else if (in.key.keycode == key_back){
            if (bar.string.size > 0){
                --bar.string.size;
            }
        }
    }
    if (in.abort) return;
    // Do the search
    buffer_search(app, bar.string, view, direction);
}

void reset_keymap_for_current_mode(struct Application_Links* app) {
    switch (state.mode) {
        case mode_normal: {
            set_current_keymap(app, mapid_normal);
        } break;

        case mode_insert: {
            set_current_keymap(app, mapid_insert);
        } break;

        case mode_replace: {
            set_current_keymap(app, mapid_replace);
        } break;

        case mode_visual_line:
        case mode_visual: {
            set_current_keymap(app, mapid_visual);
        } break;
    }
}

}  // namespace

//=============================================================================
// > Custom commands <                                                @commands
// Commands that do things.
//=============================================================================

CUSTOM_COMMAND_SIG(enter_normal_mode_on_current) {
    enter_normal_mode(app, get_current_view_buffer_id(app, AccessAll));
}

CUSTOM_COMMAND_SIG(enter_replace_mode){
    state.mode = mode_replace;
    set_current_keymap(app, mapid_replace);
    clear_register_selection();
    on_enter_replace_mode(app);
}

CUSTOM_COMMAND_SIG(enter_visual_mode){
    state.mode = mode_visual;
    state.selection_cursor.start = get_cursor_pos(app);
    state.selection_cursor.end = state.selection_cursor.start;
    update_visual_range(app, state.selection_cursor.end);

    set_current_keymap(app, mapid_visual);
    clear_register_selection();
    on_enter_visual_mode(app);
}

CUSTOM_COMMAND_SIG(enter_visual_line_mode){
    state.mode = mode_visual_line;
    state.selection_cursor.start = get_cursor_pos(app);
    state.selection_cursor.end = state.selection_cursor.start;
    update_visual_line_range(app, state.selection_cursor.end);

    set_current_keymap(app, mapid_visual);
    clear_register_selection();
    on_enter_visual_mode(app);
}

CUSTOM_COMMAND_SIG(enter_chord_replace_single){
    set_current_keymap(app, mapid_chord_replace_single);
    clear_register_selection();
}

CUSTOM_COMMAND_SIG(enter_chord_switch_registers){
    set_current_keymap(app, mapid_chord_choose_register);

    push_to_chord_bar(app, lit("\""));
}

CUSTOM_COMMAND_SIG(replace_character) {
    //TODO(chronister): Do something a little more intelligent when at the end of a line
    if (get_cursor_char(app) != '\n') {
        delete_char(app);
    }
    write_character(app);
}

CUSTOM_COMMAND_SIG(replace_character_then_normal) {
    replace_character(app);
    move_left(app);
    enter_normal_mode(app, get_current_view_buffer_id(app, AccessAll));
}

CUSTOM_COMMAND_SIG(seek_top_of_file) {
    unsigned int access = AccessProtected;
    View_Summary view = get_active_view(app, access);
    view_set_cursor(app, &view, seek_pos(0), true);
}

CUSTOM_COMMAND_SIG(seek_bottom_of_file) {
    unsigned int access = AccessProtected;
    View_Summary view = get_active_view(app, access);
    Buffer_Summary buffer = get_buffer(app, view.buffer_id, access);
    view_set_cursor(app, &view, seek_pos(buffer.size), true);
}

template <CUSTOM_COMMAND_SIG(command)>
CUSTOM_COMMAND_SIG(compound_move_command){
    View_Summary view = get_active_view(app, AccessProtected);
    int before_pos = view.cursor.pos;
    command(app);
    refresh_view(app, &view);
    int after_pos = view.cursor.pos;
    vim_exec_action(app, make_range(before_pos, after_pos), false);
}

#define vim_move_left compound_move_command<move_left>
#define vim_move_right compound_move_command<move_right>
#define vim_move_end_of_line compound_move_command<seek_end_of_line>
#define vim_move_beginning_of_line compound_move_command<seek_beginning_of_line>
#define vim_move_whitespace_up compound_move_command<seek_whitespace_up>
#define vim_move_whitespace_down compound_move_command<seek_whitespace_down>
#define vim_move_to_top compound_move_command<seek_top_of_file>
#define vim_move_to_bottom compound_move_command<seek_bottom_of_file>
#define vim_move_click compound_move_command<click_set_cursor>
#define vim_move_scroll compound_move_command<mouse_wheel_scroll>

CUSTOM_COMMAND_SIG(move_forward_word_start){
    View_Summary view;
    Buffer_Summary buffer;

    unsigned int access = AccessAll;
    view = get_active_view(app, access);
    buffer = get_buffer(app, view.buffer_id, access);

    int pos1 = view.cursor.pos;
    
    int pos2 = buffer_seek_next_word(app, &buffer, pos1);

    view_set_cursor(app, &view, seek_pos(pos2), true);
    vim_exec_action(app, make_range(pos1, pos2), false);
}

CUSTOM_COMMAND_SIG(move_backward_word_start){
    View_Summary view;
    
    unsigned int access = AccessAll;
    view = get_active_view(app, access);

    int pos1 = view.cursor.pos;
    
    seek_white_or_token_left(app);
    
    refresh_view(app, &view);
    int pos2 = view.cursor.pos;

    vim_exec_action(app, make_range(pos1, pos2));
}

CUSTOM_COMMAND_SIG(move_forward_word_end){
    View_Summary view;
    
    unsigned int access = AccessOpen;
    view = get_active_view(app, access);

    int pos1 = view.cursor.pos;
    move_right(app);
    
    seek_whitespace_right(app);
    
    refresh_view(app, &view);
    int pos2 = view.cursor.pos;
    move_left(app);

    vim_exec_action(app, make_range(pos1, pos2));
}

CUSTOM_COMMAND_SIG(newline_then_insert_before){
    seek_beginning_of_line(app);
    write_string(app, make_lit_string("\n"));
    move_left(app);
    enter_insert_mode(app, get_current_view_buffer_id(app, AccessAll));
}

CUSTOM_COMMAND_SIG(insert_at){
    enter_insert_mode(app, get_current_view_buffer_id(app, AccessAll));
}

CUSTOM_COMMAND_SIG(insert_after){
    View_Summary view = get_active_view(app, AccessOpen);
    Buffer_Summary buffer = get_buffer(app, view.buffer_id, AccessOpen);
    char nextch[2];
    int pos = view.cursor.pos;
    buffer_read_range(app, &buffer, pos, pos + 1, nextch);
    if (nextch[0] != '\n') {
        move_right(app);
    }
    enter_insert_mode(app, view.buffer_id);
}

CUSTOM_COMMAND_SIG(seek_eol_then_insert){
    seek_end_of_line(app);
    enter_insert_mode(app, get_current_view_buffer_id(app, AccessOpen));
}

CUSTOM_COMMAND_SIG(newline_then_insert_after){
    seek_end_of_line(app);
    write_string(app, make_lit_string("\n"));
    enter_insert_mode(app, get_current_view_buffer_id(app, AccessOpen));
}

CUSTOM_COMMAND_SIG(enter_chord_delete){
    set_current_keymap(app, mapid_chord_delete);

    state.action = vimaction_delete_range;

    push_to_chord_bar(app, lit("d"));
}

CUSTOM_COMMAND_SIG(enter_chord_change){
    set_current_keymap(app, mapid_chord_delete);

    state.action = vimaction_change_range;

    push_to_chord_bar(app, lit("c"));
}

CUSTOM_COMMAND_SIG(enter_chord_yank){
    set_current_keymap(app, mapid_chord_yank);

    state.action = vimaction_yank_range;

    push_to_chord_bar(app, lit("y"));
}

CUSTOM_COMMAND_SIG(enter_chord_indent_left){
    set_current_keymap(app, mapid_chord_indent_left);
    state.action = vimaction_indent_left_range;
    push_to_chord_bar(app, lit("<"));
}

CUSTOM_COMMAND_SIG(enter_chord_indent_right){
    set_current_keymap(app, mapid_chord_indent_right);
    state.action = vimaction_indent_right_range;
    push_to_chord_bar(app, lit(">"));
}

CUSTOM_COMMAND_SIG(enter_chord_format){
    set_current_keymap(app, mapid_chord_format);

    state.action = vimaction_format_range;

    push_to_chord_bar(app, lit("="));
}

CUSTOM_COMMAND_SIG(enter_chord_window){
    set_current_keymap(app, mapid_chord_window);
    push_to_chord_bar(app, lit("^W"));
}

CUSTOM_COMMAND_SIG(enter_chord_move_find){
    set_current_keymap(app, mapid_chord_move_find);
    push_to_chord_bar(app, lit("f"));
}

CUSTOM_COMMAND_SIG(enter_chord_move_til){
    set_current_keymap(app, mapid_chord_move_til);
    push_to_chord_bar(app, lit("t"));
}

CUSTOM_COMMAND_SIG(enter_chord_move_rfind){
    set_current_keymap(app, mapid_chord_move_rfind);
    push_to_chord_bar(app, lit("F"));
}

CUSTOM_COMMAND_SIG(enter_chord_move_rtil){
    set_current_keymap(app, mapid_chord_move_rtil);
    push_to_chord_bar(app, lit("T"));
}

CUSTOM_COMMAND_SIG(enter_chord_g){
    set_current_keymap(app, mapid_chord_g);
    push_to_chord_bar(app, lit("g"));
}

CUSTOM_COMMAND_SIG(move_line_exec_action){
    View_Summary view = get_active_view(app, AccessProtected);
    int initial = view.cursor.pos;
    seek_beginning_of_line(app);
    refresh_view(app, &view);
    int line_begin = view.cursor.pos;
    seek_end_of_line(app);
    refresh_view(app, &view);
    int line_end = view.cursor.pos + 1;
    vim_exec_action(app, make_range(line_begin, line_end), true);
    view_set_cursor(app, &view, seek_pos(initial), true);
}

CUSTOM_COMMAND_SIG(vim_delete_line){
    state.action = vimaction_delete_range;
    move_line_exec_action(app);
}

CUSTOM_COMMAND_SIG(yank_line){
    state.action = vimaction_yank_range;
    move_line_exec_action(app);
}

template <Search_Direction seek_forward, bool include_found>
CUSTOM_COMMAND_SIG(seek_for_character){
    Buffer_Summary buffer;
    View_Summary view;
    User_Input trigger;
    int pos1, pos2;
    
    unsigned int access = AccessProtected;
    view = get_active_view(app, access);
    buffer = get_buffer(app, view.buffer_id, access);

    trigger = get_command_input(app);

    pos1 = view.cursor.pos;
    if (seek_forward) {
        buffer_seek_delimiter_forward(app, &buffer, pos1+1, (char)trigger.key.character, &pos2);
    }
    else {
        buffer_seek_delimiter_backward(app, &buffer, pos1-1, (char)trigger.key.character, &pos2);
    }
    move_left(app);
    if (!include_found) { 
        pos2 += seek_forward;
    }
    view_set_cursor(app, &view, seek_pos(pos2), true);
    
    if (pos2 >= 0) {
        vim_exec_action(app, make_range(pos1, pos2));
    }
    else {
        //TODO(chronister): This will not be correct for visual mode!
        enter_normal_mode(app, get_current_view_buffer_id(app, AccessAll));
    }
}

#define vim_seek_find_character seek_for_character<search_forward, true>
#define vim_seek_til_character seek_for_character<search_forward, false>
#define vim_seek_rfind_character seek_for_character<search_backward, true>
#define vim_seek_rtil_character seek_for_character<search_backward, false>

//TODO(chronister): move_up and move_down both operate on lines, which is not reflected here.
CUSTOM_COMMAND_SIG(vim_move_up){
    View_Summary view;
    int pos1, pos2;
    
    unsigned int access = AccessProtected;
    view = get_active_view(app, access);

    pos1 = view.cursor.pos;

    move_up(app);
    refresh_view(app, &view);
    pos2 = view.cursor.pos;
    
    vim_exec_action(app, make_range(pos1, pos2));
}

CUSTOM_COMMAND_SIG(vim_move_down){
    View_Summary view;
    int pos1, pos2;
    
    unsigned int access = AccessProtected;
    view = get_active_view(app, access);

    pos1 = view.cursor.pos;

    move_down(app);
    refresh_view(app, &view);
    pos2 = view.cursor.pos;
    
    vim_exec_action(app, make_range(pos1, pos2));
}

CUSTOM_COMMAND_SIG(cycle_window_focus){
    // ASSUMPTION: End of a ^W window shortcut presumably
    set_current_keymap(app, mapid_normal);
    end_chord_bar(app);

    change_active_panel(app);
}

CUSTOM_COMMAND_SIG(open_window_hsplit){
    // ASSUMPTION: End of a ^W window shortcut presumably
    set_current_keymap(app, mapid_normal);
    end_chord_bar(app);

    View_Summary view = get_active_view(app, AccessAll);
    View_Summary new_view = open_view(app, &view, ViewSplit_Top);
    set_active_view(app, &view);
}

CUSTOM_COMMAND_SIG(open_window_dup_hsplit){
    // ASSUMPTION: End of a ^W window shortcut presumably
    set_current_keymap(app, mapid_normal);
    end_chord_bar(app);

    View_Summary view = get_active_view(app, AccessAll);
    View_Summary new_view = open_view(app, &view, ViewSplit_Top);
    view_set_buffer(app, &new_view, view.buffer_id, 0);
    set_active_view(app, &view);
}

CUSTOM_COMMAND_SIG(open_window_vsplit){
    // ASSUMPTION: End of a ^W window shortcut presumably
    set_current_keymap(app, mapid_normal);
    end_chord_bar(app);

    View_Summary view = get_active_view(app, AccessAll);
    View_Summary new_view = open_view(app, &view, ViewSplit_Right);
    set_active_view(app, &view);
}

CUSTOM_COMMAND_SIG(open_window_dup_vsplit){
    // ASSUMPTION: End of a ^W window shortcut presumably
    set_current_keymap(app, mapid_normal);
    end_chord_bar(app);

    View_Summary view = get_active_view(app, AccessAll);
    View_Summary new_view = open_view(app, &view, ViewSplit_Right);
    view_set_buffer(app, &new_view, view.buffer_id, 0);
    set_active_view(app, &view);
}

CUSTOM_COMMAND_SIG(focus_window_left) {
    View_Summary view = get_active_view(app, AccessAll);

    set_current_keymap(app, mapid_normal);
    end_chord_bar(app);

    i32_Rect current = view.view_region;
    int x = current.x0; //view.cursor.wrapped_x;
    int y = current.y0; //view.cursor.wrapped_y;

    View_Summary best = view;

    for_views(nextview, app) {
        if (nextview.view_id == view.view_id) continue;
        i32_Rect next = nextview.view_region;
        if (y < next.y0 || y > next.y1) continue;
        if (x < next.x0) continue;
        if (best.view_id == view.view_id || next.x0 > best.view_region.x0) best = nextview;
    }

    set_active_view(app, &best);
}

CUSTOM_COMMAND_SIG(focus_window_right) {
    View_Summary view = get_active_view(app, AccessAll);

    set_current_keymap(app, mapid_normal);
    end_chord_bar(app);

    i32_Rect current = view.view_region;
    int x = current.x0; //view.cursor.wrapped_x;
    int y = current.y0; //view.cursor.wrapped_y;

    View_Summary best = view;

    for_views(nextview, app) {
        if (nextview.view_id == view.view_id) continue;
        i32_Rect next = nextview.view_region;
        if (y < next.y0 || y > next.y1) continue;
        if (x > next.x0) continue;
        if (best.view_id == view.view_id || next.x0 < best.view_region.x0) best = nextview;
    }

    set_active_view(app, &best);
}

CUSTOM_COMMAND_SIG(focus_window_down) {
    View_Summary view = get_active_view(app, AccessAll);

    set_current_keymap(app, mapid_normal);
    end_chord_bar(app);

    i32_Rect current = view.view_region;
    int x = current.x0; //view.cursor.wrapped_x;
    int y = current.y0; //view.cursor.wrapped_y;

    View_Summary best = view;

    for_views(nextview, app) {
        if (nextview.view_id == view.view_id) continue;
        i32_Rect next = nextview.view_region;
        if (x < next.x0 || x > next.x1) continue;
        if (y < next.y0) continue;
        if (best.view_id == view.view_id || next.y0 > best.view_region.y0) best = nextview;
    }

    set_active_view(app, &best);
}

CUSTOM_COMMAND_SIG(focus_window_up) {
    View_Summary view = get_active_view(app, AccessAll);

    set_current_keymap(app, mapid_normal);
    end_chord_bar(app);

    i32_Rect current = view.view_region;
    int x = current.x0; //view.cursor.wrapped_x;
    int y = current.y0; //view.cursor.wrapped_y;

    View_Summary best = view;

    for_views(nextview, app) {
        if (nextview.view_id == view.view_id) continue;
        i32_Rect next = nextview.view_region;
        if (x < next.x0 || x > next.x1) continue;
        if (y > next.y0) continue;
        if (best.view_id == view.view_id || next.y0 < best.view_region.y0) best = nextview;
    }

    set_active_view(app, &best);
}

CUSTOM_COMMAND_SIG(close_window) {
    set_current_keymap(app, mapid_normal);
    end_chord_bar(app);

    View_Summary view = get_view_first(app, AccessAll);
    get_view_next(app, &view, AccessAll);
    if (!view.exists) send_exit_signal(app);
    else close_panel(app);
}

CUSTOM_COMMAND_SIG(combine_with_next_line) {
    seek_end_of_line(app);
    delete_char(app);
}

CUSTOM_COMMAND_SIG(paste_before_cursor_char) {
    View_Summary view;
    Buffer_Summary buffer;
    
    unsigned int access = AccessOpen;
    view = get_active_view(app, access);
    buffer = get_buffer(app, view.buffer_id, access);

    Vim_Register* reg = state.registers + state.paste_register;
    if (reg->is_line) {
        seek_beginning_of_line(app);
        refresh_view(app, &view);
        int paste_pos = view.cursor.pos;
        paste_from_register(app, &buffer, paste_pos, reg); 
        view_set_cursor(app, &view, seek_pos(paste_pos), true);
    } else {
        int paste_pos = view.cursor.pos;
        paste_from_register(app, &buffer, paste_pos, reg); 
        view_set_cursor(app, &view, seek_pos(paste_pos + reg->text.size - 1),
                        true);
    }
    clear_register_selection();
}

CUSTOM_COMMAND_SIG(paste_after_cursor_char) {
    View_Summary view;
    Buffer_Summary buffer;
    
    unsigned int access = AccessOpen;
    view = get_active_view(app, access);
    buffer = get_buffer(app, view.buffer_id, access);

    Vim_Register* reg = state.registers + state.paste_register;
    if (reg->is_line) {
        seek_end_of_line(app);
        move_right(app);
        refresh_view(app, &view);
        int paste_pos = view.cursor.pos;
        paste_from_register(app, &buffer, paste_pos, reg); 
        view_set_cursor(app, &view, seek_pos(paste_pos), true);
    } else {
        int paste_pos = view.cursor.pos + 1;
        paste_from_register(app, &buffer, paste_pos, reg); 
        view_set_cursor(app, &view, seek_pos(paste_pos + reg->text.size - 1),
                        true);
    }
    clear_register_selection();
}

CUSTOM_COMMAND_SIG(visual_delete) {
    state.action = vimaction_delete_range;
    vim_exec_action(app, state.selection_range, state.mode == mode_visual_line);
    enter_normal_mode(app, get_current_view_buffer_id(app, AccessAll));
}

CUSTOM_COMMAND_SIG(visual_change) {
    state.action = vimaction_change_range;
    vim_exec_action(app, state.selection_range, state.mode == mode_visual_line);
    enter_normal_mode(app, get_current_view_buffer_id(app, AccessAll));
}

CUSTOM_COMMAND_SIG(visual_yank) {
    state.action = vimaction_yank_range;
    vim_exec_action(app, state.selection_range, state.mode == mode_visual_line);
    enter_normal_mode(app, get_current_view_buffer_id(app, AccessAll));
}

CUSTOM_COMMAND_SIG(visual_format) {
    state.action = vimaction_format_range;
    vim_exec_action(app, state.selection_range, state.mode == mode_visual_line);
    enter_normal_mode(app, get_current_view_buffer_id(app, AccessAll));
}

CUSTOM_COMMAND_SIG(visual_indent_right) {
    state.action = vimaction_format_range; // TODO(chr)
    vim_exec_action(app, state.selection_range, state.mode == mode_visual_line);
    enter_normal_mode(app, get_current_view_buffer_id(app, AccessAll));
}

CUSTOM_COMMAND_SIG(visual_indent_left) {
    state.action = vimaction_format_range; // TODO(chr)
    vim_exec_action(app, state.selection_range, state.mode == mode_visual_line);
    enter_normal_mode(app, get_current_view_buffer_id(app, AccessAll));
}

CUSTOM_COMMAND_SIG(select_register) {
    User_Input trigger;
    trigger = get_command_input(app);

    Register_Id regid = regid_from_char(trigger.key.character);
    if (regid == reg_unnamed) {
        enter_normal_mode(app, get_current_view_buffer_id(app, AccessAll));
    }

    state.yank_register = state.paste_register = regid;
    char str[2] = { (char)trigger.key.character, '\0' };
    push_to_chord_bar(app, lit(str));

    reset_keymap_for_current_mode(app);
}

CUSTOM_COMMAND_SIG(vim_open_file_in_quotes){
    // @COPYPASTA from 4coder_default_include.cpp
    View_Summary view;
    Buffer_Summary buffer;
    char short_file_name[128];
    int pos, start, end, size;
    
    unsigned int access = AccessProtected;
    view = get_active_view(app, access);
    buffer = get_buffer(app, view.buffer_id, access);
    pos = view.cursor.pos;
    buffer_seek_delimiter_forward(app, &buffer, pos + 1, '"', &end);
    buffer_seek_delimiter_backward(app, &buffer, pos, '"', &start);
    
    ++start;
    size = end - start;

    end_chord_bar(app);
    enter_normal_mode(app, get_current_view_buffer_id(app, AccessAll));
    
    // NOTE(allen): This check is necessary because buffer_read_range
    // requires that the output buffer you provide is at least (end - start) bytes long.
    if (size < sizeof(short_file_name)){
        char file_name_[256];
        String file_name = make_fixed_width_string(file_name_);
        
        buffer_read_range(app, &buffer, start, end, short_file_name);
        
        copy(&file_name, make_string(buffer.file_name, buffer.file_name_len));
        remove_last_folder(&file_name);
        append(&file_name, make_string(short_file_name, size));
        
        view_open_file(app, &view, expand_str(file_name), false);
    }
}

CUSTOM_COMMAND_SIG(search_under_cursor) {
    View_Summary view;
    Buffer_Summary buffer;
    view = get_active_view(app, AccessAll);
    buffer = get_buffer(app, view.buffer_id, AccessAll);
    if (!buffer.exists) return;
    Range word = get_word_under_cursor(app, &buffer, &view);
    char* wordStr = (char*)malloc(word.end - word.start);
    defer(free(wordStr));
    buffer_read_range(app, &buffer, word.start, word.end, wordStr);
    buffer_search(app, make_string(wordStr, word.end - word.start), view,
                  search_forward);
}

CUSTOM_COMMAND_SIG(vim_search) {
    buffer_query_search(app, search_forward);
}

CUSTOM_COMMAND_SIG(vim_search_reverse) {
    buffer_query_search(app, search_backward);
}

CUSTOM_COMMAND_SIG(vim_search_next) {
    View_Summary view = get_active_view(app, AccessAll);
    buffer_search(app, state.last_search.text, view,
                  state.last_search.direction);
}

CUSTOM_COMMAND_SIG(vim_search_prev) {
    View_Summary view = get_active_view(app, AccessAll);
    Search_Direction current_direction = state.last_search.direction;
    buffer_search(app, state.last_search.text, view,
                  (Search_Direction)(-current_direction));
    // Preserve search direction
    state.last_search.direction = current_direction;
}

CUSTOM_COMMAND_SIG(vim_delete_char) {
    View_Summary view = get_active_view(app, AccessOpen);
    if (!view.exists) { return; }
    Buffer_Summary buffer = get_buffer(app, view.buffer_id, AccessOpen);
    
    char nextch[2];
    int pos = view.cursor.pos;
    buffer_read_range(app, &buffer, pos, pos + 1, nextch);
    if (nextch[0] != '\n') {
        if (0 < buffer.size && pos < buffer.size){
            buffer_replace_range(app, &buffer,
                                      pos, pos+1, 0, 0);
            // TODO(chr): Going into register?
        }
    }
}

// TODO(chr): Measure the lister size?
constexpr int HALF_PAGE = 5;

CUSTOM_COMMAND_SIG(lister__page_down) {
    for (int i = 0; i < HALF_PAGE; ++i) {
        lister__move_down(app);
    }
}

CUSTOM_COMMAND_SIG(lister__page_up) {
    for (int i = 0; i < HALF_PAGE; ++i) {
        lister__move_up(app);
    }
}

//=============================================================================
// > Statusbar processing and commands <                             @statusbar
// This is where the vim statusbar feature is created.
//
// Define a command with VIM_COMMAND_FUNC_SIG and then add it to the statusbar
// library with define_command().
//=============================================================================

CUSTOM_COMMAND_SIG(status_command){
    User_Input in;
    Query_Bar bar;

    set_current_keymap(app, mapid_normal);

    if (start_query_bar(app, &bar, 0) == 0) return;
    defer(end_query_bar(app, &bar, 0));

    char bar_string_space[256];
    bar.string = make_fixed_width_string(bar_string_space);

    bar.prompt = make_lit_string(":");

    while (1){
        in = get_user_input(app, EventOnAnyKey, EventOnEsc);
        if (in.abort) break;
        if (in.key.keycode == '\n'){
            break;
        }
        else if (in.key.keycode == '\t') {
            // Ignore it for now
            // TODO(chronister): auto completion!
        }
        else if (in.key.character && key_is_unmodified(&in.key)){
            append(&bar.string, (char)in.key.character);
        }
        else if (in.key.keycode == key_back){
            if (bar.string.size > 0){
                --bar.string.size;
            }
        }

        // TODO(chr): Make these hookable so users can make their own
        // interactive stuff
        if (match(bar.string, make_lit_string("e "))) {
            exec_command(app, interactive_open);
            return;
        }

        if (match(bar.string, make_lit_string("b "))) {
            exec_command(app, interactive_switch_buffer);
            return;
        }

        if (match(bar.string, make_lit_string("bw "))) {
            exec_command(app, interactive_kill_buffer);
            return;
        }
    }
    if (in.abort) return;

    int command_offset = 0;
    while (command_offset < bar.string.size && 
           char_is_whitespace(bar.string.str[command_offset])) {
        ++command_offset;
    }

    int command_end = command_offset;
    while (command_end < bar.string.size &&
           !char_is_whitespace(bar.string.str[command_end])) {
        ++command_end;
    }
    
    if (command_end == command_offset) { return; }
    String command = substr(bar.string, command_offset, command_end - command_offset);
    bool command_force = false;
    if (command.str[command.size - 1] == '!') {
        command.size -= 1;
        command_force = true;
    }

    bool command_is_numeric = true;
    for (int command_ch = 0; command_ch < command.size; ++command_ch) {
        if (!('0' <= command.str[command_ch] && command.str[command_ch] <= '9')) {
            command_is_numeric = false;
        }
    }

    if (command_is_numeric) {
        int line = str_to_int(command);
        active_view_to_line(app, line);
        return;
    }

    int arg_start = command_end;
    while (arg_start < bar.string.size && 
           char_is_whitespace(bar.string.str[arg_start])) {
        ++arg_start;
    }
    String argstr = substr(bar.string, arg_start, bar.string.size - arg_start);

    for (int command_index = 0; command_index < defined_command_count; ++command_index) {
        Vim_Command_Defn defn = defined_commands[command_index];
        if (match_part(defn.command, command)) {
            defn.func(app, command, argstr, command_force);
            break;
        }
    }
}

void define_command(String command, Vim_Command_Func func) {
    Vim_Command_Defn* defn = defined_commands + defined_command_count++;
    defn->command = command;
    defn->func = func;
}

VIM_COMMAND_FUNC_SIG(write_file) {
    View_Summary view = get_active_view(app, AccessProtected);
    Buffer_Summary buffer = get_buffer(app, view.buffer_id, AccessProtected);
    if (argstr.str == NULL || argstr.size == 0) {
        save_buffer(app, &buffer, buffer.file_name, buffer.file_name_len, 0);
    } else {
        save_buffer(app, &buffer, expand_str(argstr), 0);
    }
}

VIM_COMMAND_FUNC_SIG(edit_file) {
    exec_command(app, interactive_open);
}

VIM_COMMAND_FUNC_SIG(new_file) {
    View_Summary view = get_active_view(app, AccessAll);
    View_Summary new_view = open_view(app, &view, ViewSplit_Top);
    new_view_settings(app, &new_view);
    set_active_view(app, &new_view);
    if (compare(argstr, make_lit_string("")) == 0) {
        exec_command(app, interactive_new);
    } else {
        Buffer_Summary buffer = create_buffer(app, argstr.str, argstr.size, 0);
        if (buffer.exists){
            view_set_buffer(app, &new_view, buffer.buffer_id, SetBuffer_KeepOriginalGUI);
        }
    }
}

VIM_COMMAND_FUNC_SIG(new_file_open_vertical) {
    View_Summary view = get_active_view(app, AccessAll);
    View_Summary new_view = open_view(app, &view, ViewSplit_Right);
    new_view_settings(app, &new_view);
    set_active_view(app, &new_view);
    exec_command(app, interactive_new);
}

VIM_COMMAND_FUNC_SIG(colorscheme) {
    if (argstr.str && argstr.size > 0) {
        change_theme(app, expand_str(argstr));
    }
    // else set bar text (...) to current colorscheme
    else {
        exec_command(app, open_color_tweaker);
    }
}

VIM_COMMAND_FUNC_SIG(close_view) {
    View_Summary view = get_view_first(app, AccessAll);
    get_view_next(app, &view, AccessAll);
    if (!view.exists) send_exit_signal(app);
    else close_panel(app);
}

VIM_COMMAND_FUNC_SIG(close_all) {
    send_exit_signal(app);
}

VIM_COMMAND_FUNC_SIG(write_file_and_close_all) {
    write_file(app, command, argstr, force);
    close_all(app, command, argstr, force);
}

VIM_COMMAND_FUNC_SIG(write_file_and_close_view) {
    write_file(app, command, argstr, force);
    close_view(app, command, argstr, force);
}

VIM_COMMAND_FUNC_SIG(vertical_split) {
    View_Summary view = get_active_view(app, AccessAll);
    View_Summary new_view = open_view(app, &view, ViewSplit_Right);
    view_set_buffer(app, &new_view, view.buffer_id, 0);
    set_active_view(app, &view);
}

VIM_COMMAND_FUNC_SIG(horizontal_split) {
    View_Summary view = get_active_view(app, AccessAll);
    View_Summary new_view = open_view(app, &view, ViewSplit_Right);
    view_set_buffer(app, &new_view, view.buffer_id, 0);
    set_active_view(app, &view);
}

VIM_COMMAND_FUNC_SIG(exec_regex) {
    fprintf(stderr, "%.*s", (int)argstr.size, argstr.str);
}

VIM_COMMAND_FUNC_SIG(change_directory) {
    char dir[4096];
    String dirstr = make_fixed_width_string(dir);
    dirstr.size = directory_get_hot(app, dirstr.str, dirstr.memory_size);
    assert(dirstr.size < 4096);
    if (!directory_cd_expand_user(app, dirstr.str, &dirstr.size, dirstr.memory_size,
                                  argstr.str, argstr.size)) {
        fprintf(stderr, "Couldn't change directory to %.*s\n", argstr.size, argstr.str);
        return;
    }
    fprintf(stderr, "%.*s\n", (int)dirstr.size, dirstr.str);
    directory_set_hot(app, dirstr.str, dirstr.size);
}

//=============================================================================
// > 4coder Hooks <                                                      @hooks
// Vim's implementation for the important 4coder hooks
//
// You need to call these from your implementations of these hooks for 4vim to
// work correctly!
//=============================================================================

// CALL ME
// This function should be called from your 4coder custom init hook
START_HOOK_SIG(vim_hook_init_func) {
    // First file replaces scratch buffer
    if (file_count > 0) {
        View_Summary view = get_active_view(app, AccessAll);
        Buffer_Summary buffer = create_buffer(app, files[0], (int32_t)strlen(files[0]), 0);
        if (buffer.exists){
            view_set_buffer(app, &view, buffer.buffer_id, 0);
        }
    }
    // Rest of files open in splits
    // TODO(chr): Emulate vim behavior here? IIRC vim will queue them up and
    // edit them one by one.
    for (int file_index = 1; file_index < file_count; ++file_index) {
        new_file(app, lit("new"), make_string(files[file_index],
                                              (i32_4tech)strlen(files[file_index])), true);
    }
    return 0;
}

// CALL ME
// This function should be called from your 4coder custom open file hook
OPEN_FILE_HOOK_SIG(vim_hook_open_file_func) {
    enter_normal_mode(app, buffer_id);
    default_file_settings(app, buffer_id);
    return 0;
}

// CALL ME
// This function should be called from your 4coder custom new file hook
OPEN_FILE_HOOK_SIG(vim_hook_new_file_func) {
    enter_normal_mode(app, buffer_id);
    return 0;
}

// CALL ME
// This function should be called from your 4coder render caller to draw the
// vim-related things on screen.
RENDER_CALLER_SIG(vim_render_caller) {
    // TODO(chr): Mostly copied from the default render caller. Customize for vim stuff.
    View_Summary view = get_view(app, view_id, AccessAll);
    Buffer_Summary buffer = get_buffer(app, view.buffer_id, AccessAll);
    View_Summary active_view = get_active_view(app, AccessAll);
    bool32 is_active_view = (active_view.view_id == view_id);
    
    static Managed_Scope render_scope = 0;
    if (render_scope == 0){
        render_scope = create_user_managed_scope(app);
    }
    
    Partition *scratch = &global_part;
    
    // NOTE(allen): Scan for TODOs and NOTEs
    {
        Theme_Color colors[2];
        colors[0].tag = Stag_Text_Cycle_2;
        colors[1].tag = Stag_Text_Cycle_1;
        get_theme_colors(app, colors, 2);
        
        Temp_Memory temp = begin_temp_memory(scratch);
        int32_t text_size = on_screen_range.one_past_last - on_screen_range.first;
        char *text = push_array(scratch, char, text_size);
        buffer_read_range(app, &buffer, on_screen_range.first, on_screen_range.one_past_last, text);
        
        Highlight_Record *records = push_array(scratch, Highlight_Record, 0);
        String tail = make_string(text, text_size);
        for (int32_t i = 0; i < text_size; tail.str += 1, tail.size -= 1, i += 1){
            if (match_part(tail, make_lit_string("NOTE"))){
                Highlight_Record *record = push_array(scratch, Highlight_Record, 1);
                record->first = i + on_screen_range.first;
                record->one_past_last = record->first + 4;
                record->color = colors[0].color;
                tail.str += 3;
                tail.size -= 3;
                i += 3;
            }
            else if (match_part(tail, make_lit_string("TODO"))){
                Highlight_Record *record = push_array(scratch, Highlight_Record, 1);
                record->first = i + on_screen_range.first;
                record->one_past_last = record->first + 4;
                record->color = colors[1].color;
                tail.str += 3;
                tail.size -= 3;
                i += 3;
            }
        }
        int32_t record_count = (int32_t)(push_array(scratch, Highlight_Record, 0) - records);
        push_array(scratch, Highlight_Record, 1);
        
        if (record_count > 0){
            sort_highlight_record(records, 0, record_count);
            Temp_Memory marker_temp = begin_temp_memory(scratch);
            Marker *markers = push_array(scratch, Marker, 0);
            int_color current_color = records[0].color;
            {
                Marker *marker = push_array(scratch, Marker, 2);
                marker[0].pos = records[0].first;
                marker[1].pos = records[0].one_past_last;
            }
            for (int32_t i = 1; i <= record_count; i += 1){
                bool32 do_emit = i == record_count || (records[i].color != current_color);
                if (do_emit){
                    int32_t marker_count = (int32_t)(push_array(scratch, Marker, 0) - markers);
                    Managed_Object o = alloc_buffer_markers_on_buffer(app, buffer.buffer_id, marker_count, &render_scope);
                    managed_object_store_data(app, o, 0, marker_count, markers);
                    Marker_Visual v = create_marker_visual(app, o);
                    marker_visual_set_effect(app, v,
                                             VisualType_CharacterHighlightRanges,
                                             SymbolicColor_Transparent, current_color, 0);
                    marker_visual_set_priority(app, v, VisualPriority_Lowest);
                    end_temp_memory(marker_temp);
                    current_color = records[i].color;
                }
                
                Marker *marker = push_array(scratch, Marker, 2);
                marker[0].pos = records[i].first;
                marker[1].pos = records[i].one_past_last;
            }
        }
        
        end_temp_memory(temp);
    }
    
    // NOTE(chr): Visual range highlight
    {
        Managed_Object highlight_range = alloc_buffer_markers_on_buffer(app, buffer.buffer_id, 2, &render_scope);
        Marker cm_markers[2] = {};
        cm_markers[0].pos = state.selection_range.start;
        cm_markers[1].pos = state.selection_range.end;
        managed_object_store_data(app, highlight_range, 0, 2, cm_markers);

        Theme_Color color = {};
        color.tag = Stag_Highlight;
        get_theme_colors(app, &color, 1);

        Marker_Visual visual = create_marker_visual(app, highlight_range);
        marker_visual_set_effect(app, visual, VisualType_CharacterHighlightRanges,
                                 color.color, 0, 0);
        Marker_Visual_Take_Rule take_rule = {};
        take_rule.first_index = 0;
        take_rule.take_count_per_step = 2;
        take_rule.step_stride_in_marker_count = 1;
        take_rule.maximum_number_of_markers = 2;
        marker_visual_set_take_rule(app, visual, take_rule);
        marker_visual_set_priority(app, visual, VisualPriority_Highest);
    }
    
    // NOTE(allen): Cursor and mark
    Managed_Object cursor_and_mark = alloc_buffer_markers_on_buffer(app, buffer.buffer_id, 2, &render_scope);
    Marker cm_markers[2] = {};
    cm_markers[0].pos = view.cursor.pos;
    cm_markers[1].pos = view.mark.pos;
    managed_object_store_data(app, cursor_and_mark, 0, 2, cm_markers);
    
    bool32 cursor_is_hidden_in_this_view = (cursor_is_hidden && is_active_view);
    if (!cursor_is_hidden_in_this_view){
        Theme_Color colors[2] = {};
        colors[0].tag = Stag_Cursor;
        colors[1].tag = Stag_Mark;
        get_theme_colors(app, colors, 2);
        int_color cursor_color = SymbolicColorFromPalette(Stag_Cursor);
        int_color mark_color   = SymbolicColorFromPalette(Stag_Mark);
        int_color text_color    = is_active_view?
            SymbolicColorFromPalette(Stag_At_Cursor):SymbolicColorFromPalette(Stag_Default);
        
        Marker_Visual_Take_Rule take_rule = {};
        take_rule.first_index = 0;
        take_rule.take_count_per_step = 1;
        take_rule.step_stride_in_marker_count = 1;
        take_rule.maximum_number_of_markers = 1;
        
        Marker_Visual visual = create_marker_visual(app, cursor_and_mark);
        Marker_Visual_Type type = is_active_view?VisualType_CharacterBlocks:VisualType_CharacterWireFrames;
        marker_visual_set_effect(app, visual,
                                 type, cursor_color, text_color, 0);
        marker_visual_set_take_rule(app, visual, take_rule);
        marker_visual_set_priority(app, visual, VisualPriority_Highest);
        
        visual = create_marker_visual(app, cursor_and_mark);
        marker_visual_set_effect(app, visual,
                                 VisualType_CharacterWireFrames, mark_color, 0, 0);
        take_rule.first_index = 1;
        marker_visual_set_take_rule(app, visual, take_rule);
        marker_visual_set_priority(app, visual, VisualPriority_Highest);
    }
    
    // NOTE(allen): Matching enclosure highlight setup
    static const int32_t color_count = 4;
    if (do_matching_enclosure_highlight){
        Theme_Color theme_colors[color_count];
        int_color colors[color_count];
        for (int32_t i = 0; i < 4; i += 1){
            theme_colors[i].tag = Stag_Back_Cycle_1 + i;
        }
        get_theme_colors(app, theme_colors, color_count);
        for (int32_t i = 0; i < 4; i += 1){
            colors[i] = theme_colors[i].color;
        }
        mark_enclosures(app, scratch, render_scope,
                        &buffer, view.cursor.pos, FindScope_Brace,
                        VisualType_LineHighlightRanges,
                        colors, 0, color_count);
    }
    if (do_matching_paren_highlight){
        Theme_Color theme_colors[color_count];
        int_color colors[color_count];
        for (int32_t i = 0; i < 4; i += 1){
            theme_colors[i].tag = Stag_Text_Cycle_1 + i;
        }
        get_theme_colors(app, theme_colors, color_count);
        for (int32_t i = 0; i < 4; i += 1){
            colors[i] = theme_colors[i].color;
        }
        int32_t pos = view.cursor.pos;
        if (buffer_get_char(app, &buffer, pos) == '('){
            pos += 1;
        }
        else if (pos > 0){
            if (buffer_get_char(app, &buffer, pos - 1) == ')'){
                pos -= 1;
            }
        }
        mark_enclosures(app, scratch, render_scope,
                        &buffer, pos, FindScope_Paren,
                        VisualType_CharacterBlocks,
                        0, colors, color_count);
    }
    
    do_core_render(app);
    
    managed_scope_clear_self_all_dependent_scopes(app, render_scope);
}

// CALL ME
// This function should be called from your 4coder custom get bindings hook
void vim_get_bindings(Bind_Helper* context) {

    set_scroll_rule(context, smooth_scroll_rule);

    // SECTION: Vim commands

    define_command(lit("s"), exec_regex);
    define_command(lit("write"), write_file);
    define_command(lit("quit"), close_view);
    define_command(lit("quitall"), close_all);
    define_command(lit("qa"), close_all);
    define_command(lit("exit"), write_file_and_close_view);
    define_command(lit("x"), write_file_and_close_view);
    define_command(lit("wq"), write_file_and_close_view);
    define_command(lit("exitall"), write_file_and_close_view);
    define_command(lit("xa"), write_file_and_close_all);
    define_command(lit("wqa"), write_file_and_close_all);
    define_command(lit("close"), close_view);
    define_command(lit("edit"), edit_file);
    define_command(lit("new"), new_file);
    define_command(lit("vnew"), new_file_open_vertical);
    define_command(lit("colorscheme"), colorscheme);
    define_command(lit("vs"), vertical_split);
    define_command(lit("vsplit"), vertical_split);
    define_command(lit("sp"), horizontal_split);
    define_command(lit("split"), horizontal_split);
    define_command(lit("cd"), change_directory);

    // SECTION: Vim keybindings

    // Movements.
    // They move the cursor around.
    // They're useful in a few different modes, so we have
    // them defined globally for other modes to inherit from.
    begin_map(context, mapid_movements);
    bind_vanilla_keys(context, cmdid_null);

    bind(context, key_esc, MDFR_NONE, enter_normal_mode_on_current);
    bind(context, key_esc, MDFR_CTRL, enter_normal_mode_on_current);
    bind(context, key_esc, MDFR_SHIFT, enter_normal_mode_on_current);

    bind(context, 'h', MDFR_NONE, vim_move_left);
    bind(context, 'j', MDFR_NONE, vim_move_down);
    bind(context, 'k', MDFR_NONE, vim_move_up);
    bind(context, 'l', MDFR_NONE, vim_move_right);

    bind(context, 'w', MDFR_NONE, move_forward_word_start);
    bind(context, 'e', MDFR_NONE, move_forward_word_end);
    bind(context, 'b', MDFR_NONE, move_backward_word_start);

    bind(context, 'f', MDFR_NONE, enter_chord_move_find);
    bind(context, 't', MDFR_NONE, enter_chord_move_til);
    bind(context, 'F', MDFR_NONE, enter_chord_move_rfind);
    bind(context, 'T', MDFR_NONE, enter_chord_move_rtil);

    bind(context, '$', MDFR_NONE, vim_move_end_of_line);
    bind(context, '0', MDFR_NONE, vim_move_beginning_of_line);
    bind(context, '{', MDFR_NONE, vim_move_whitespace_up);
    bind(context, '}', MDFR_NONE, vim_move_whitespace_down);

    bind(context, 'G', MDFR_NONE, vim_move_to_bottom);

    bind(context, '*', MDFR_NONE, search_under_cursor);

    bind(context, '/', MDFR_NONE, vim_search);
    bind(context, '?', MDFR_NONE, vim_search_reverse);
    bind(context, 'n', MDFR_NONE, vim_search_next);
    bind(context, 'N', MDFR_NONE, vim_search_prev);

    bind(context, key_mouse_left, MDFR_NONE, vim_move_click);
    bind(context, key_mouse_wheel, MDFR_NONE, vim_move_scroll);

    // Include status command thingy here so that you can do commands in any non-inserty mode
    bind(context, ':', MDFR_NONE, status_command);
    end_map(context);

    // Normal mode.
    // aka "It's eating all my input, help!" mode.
    // Shortcuts for navigation, entering various modes,
    // dealing with the editor.
    begin_map(context, mapid_normal);
    inherit_map(context, mapid_movements);

    bind(context, 'J', MDFR_NONE, combine_with_next_line);

    // TODO(chr): Hitting top/bottom of file if near them
    bind(context, 'u', MDFR_CTRL, page_up);
    bind(context, 'd', MDFR_CTRL, page_down);

    // TODO(chr): this doesn't go into register like you want
    bind(context, 'x', MDFR_NONE, vim_delete_char);
    bind(context, 'P', MDFR_NONE, paste_before_cursor_char);
    bind(context, 'p', MDFR_NONE, paste_after_cursor_char);

    bind(context, 'u', MDFR_NONE, cmdid_undo);
    bind(context, 'r', MDFR_CTRL, cmdid_redo);

    bind(context, 'i', MDFR_NONE, insert_at);
    bind(context, 'a', MDFR_NONE, insert_after);
    bind(context, 'A', MDFR_NONE, seek_eol_then_insert);
    bind(context, 'o', MDFR_NONE, newline_then_insert_after);
    bind(context, 'O', MDFR_NONE, newline_then_insert_before);
    bind(context, 'r', MDFR_NONE, enter_chord_replace_single);
    bind(context, 'R', MDFR_NONE, enter_replace_mode);
    bind(context, 'v', MDFR_NONE, enter_visual_mode);
    bind(context, 'V', MDFR_NONE, enter_visual_line_mode);

    // TODO(chr): Proper alphabetic marks
    bind(context, 'm', MDFR_NONE, set_mark);
    bind(context, '`', MDFR_NONE, cursor_mark_swap);

    bind(context, '"', MDFR_NONE, enter_chord_switch_registers);

    bind(context, 'd', MDFR_NONE, enter_chord_delete);
    bind(context, 'c', MDFR_NONE, enter_chord_change);
    bind(context, 'y', MDFR_NONE, enter_chord_yank);
    bind(context, '>', MDFR_NONE, enter_chord_indent_right);
    bind(context, '<', MDFR_NONE, enter_chord_indent_left);
    bind(context, '=', MDFR_NONE, enter_chord_format);
    bind(context, 'g', MDFR_NONE, enter_chord_g);
    bind(context, 'w', MDFR_CTRL, enter_chord_window);
    bind(context, 'D', MDFR_NONE, vim_delete_line);
    bind(context, 'Y', MDFR_NONE, yank_line);

    end_map(context);

    begin_map(context, mapid_unbound);
    inherit_map(context, mapid_movements);
    bind(context, ':', MDFR_NONE, status_command);
    end_map(context);

    // Visual mode
    // aka "Selecting stuff" mode
    // A very useful mode!
    begin_map(context, mapid_visual);
    inherit_map(context, mapid_movements);
    bind(context, 'u', MDFR_CTRL, page_up);
    bind(context, 'd', MDFR_CTRL, page_down);
    bind(context, '"', MDFR_NONE, enter_chord_switch_registers);
    bind(context, 'd', MDFR_NONE, visual_delete);
    bind(context, 'x', MDFR_NONE, visual_delete);
    bind(context, 'c', MDFR_NONE, visual_change);
    bind(context, 'y', MDFR_NONE, visual_yank);
    bind(context, '=', MDFR_NONE, visual_format);
    bind(context, '>', MDFR_NONE, visual_indent_right);
    bind(context, '<', MDFR_NONE, visual_indent_left);
    end_map(context);

    // Insert mode
    // You type and it goes into the buffer. Nice and simple.
    // Escape to exit.
    begin_map(context, mapid_insert);
    inherit_map(context, mapid_nomap);

    bind_vanilla_keys(context, write_character);
    bind(context, ' ', MDFR_SHIFT, write_character);
    bind(context, key_back, MDFR_NONE, backspace_char);
    bind(context, 'n', MDFR_CTRL, word_complete);

    bind(context, key_esc, MDFR_NONE, enter_normal_mode_on_current);
    bind(context, key_esc, MDFR_SHIFT, enter_normal_mode_on_current);
    bind(context, key_esc, MDFR_CTRL, enter_normal_mode_on_current);
    bind(context, key_esc, MDFR_ALT, enter_normal_mode_on_current);

    end_map(context);

    // Replace mode
    // You type and it goes into the buffer. Nice and simple.
    // Escape to exit.
    begin_map(context, mapid_replace);
    inherit_map(context, mapid_nomap);

    bind_vanilla_keys(context, replace_character);
    bind(context, ' ', MDFR_SHIFT, write_character);
    bind(context, key_back, MDFR_NONE, backspace_char);
    bind(context, 'n', MDFR_CTRL, word_complete);

    bind(context, key_esc, MDFR_NONE, enter_normal_mode_on_current);

    end_map(context);

    // Chord "modes".
    // They're not really an explicit mode per-say, but the meaning of key presses
    // does change once a chord starts, and is context-dependent.
    
    // Single-char replace mode
    begin_map(context, mapid_chord_replace_single);
    inherit_map(context, mapid_nomap);
    bind_vanilla_keys(context, replace_character_then_normal);
    bind(context, key_esc, MDFR_NONE, enter_normal_mode_on_current);
    end_map(context);
    
    // Choosing register for yank/paste chords
    begin_map(context, mapid_chord_choose_register);
    inherit_map(context, mapid_nomap);
    bind_vanilla_keys(context, select_register);
    bind(context, key_esc, MDFR_NONE, enter_normal_mode_on_current);
    end_map(context);

    // Move-find chords
    begin_map(context, mapid_chord_move_find);
    inherit_map(context, mapid_nomap);
    bind_vanilla_keys(context, vim_seek_find_character);
    bind(context, key_esc, MDFR_NONE, enter_normal_mode_on_current);
    end_map(context);

    // Move-til chords
    begin_map(context, mapid_chord_move_til);
    bind_vanilla_keys(context, vim_seek_til_character);
    inherit_map(context, mapid_nomap);
    bind(context, key_esc, MDFR_NONE, enter_normal_mode_on_current);
    end_map(context);

    // Move-rfind chords
    begin_map(context, mapid_chord_move_rfind);
    inherit_map(context, mapid_nomap);
    bind_vanilla_keys(context, vim_seek_rfind_character);
    bind(context, key_esc, MDFR_NONE, enter_normal_mode_on_current);
    end_map(context);

    // Move-rtil chords
    begin_map(context, mapid_chord_move_rtil);
    bind_vanilla_keys(context, vim_seek_rtil_character);
    inherit_map(context, mapid_nomap);
    bind(context, key_esc, MDFR_NONE, enter_normal_mode_on_current);
    end_map(context);

    // Delete+movement chords
    begin_map(context, mapid_chord_delete);
    inherit_map(context, mapid_movements);
    bind(context, 'd', MDFR_NONE, move_line_exec_action);
    bind(context, 'c', MDFR_NONE, move_line_exec_action);
    end_map(context);

    // yank+movement chords
    begin_map(context, mapid_chord_yank);
    inherit_map(context, mapid_movements);
    bind(context, 'y', MDFR_NONE, move_line_exec_action);
    end_map(context);

    // indent+movement chords
    begin_map(context, mapid_chord_indent_left);
    inherit_map(context, mapid_movements);
    bind(context, '<', MDFR_NONE, move_line_exec_action);
    end_map(context);

    begin_map(context, mapid_chord_indent_right);
    inherit_map(context, mapid_movements);
    bind(context, '>', MDFR_NONE, move_line_exec_action);
    end_map(context);

    // format+movement chords
    begin_map(context, mapid_chord_format);
    inherit_map(context, mapid_movements);
    bind(context, '=', MDFR_NONE, move_line_exec_action);
    end_map(context);

    // Map for chords which start with the letter g
    begin_map(context, mapid_chord_g);
    inherit_map(context, mapid_nomap);

    bind(context, 'g', MDFR_NONE, vim_move_to_top);
    bind(context, 'f', MDFR_NONE, vim_open_file_in_quotes);

    //TODO(chronister): Folds!

    bind(context, key_esc, MDFR_NONE, enter_normal_mode_on_current);
    end_map(context);

    // Window navigation/manipulation chords
    begin_map(context, mapid_chord_window);
    inherit_map(context, mapid_nomap);

    bind(context, 'w', MDFR_NONE, cycle_window_focus);
    bind(context, 'w', MDFR_CTRL, cycle_window_focus);
    bind(context, 'v', MDFR_NONE, open_window_dup_vsplit);
    bind(context, 'v', MDFR_CTRL, open_window_dup_vsplit);
    bind(context, 's', MDFR_NONE, open_window_dup_hsplit);
    bind(context, 's', MDFR_CTRL, open_window_dup_hsplit);
    bind(context, 'n', MDFR_NONE, open_window_hsplit);
    bind(context, 'n', MDFR_CTRL, open_window_hsplit);
    bind(context, 'q', MDFR_NONE, close_window);
    bind(context, 'q', MDFR_CTRL, close_window);
    bind(context, 'h', MDFR_NONE, focus_window_left);
    bind(context, 'h', MDFR_CTRL, focus_window_left);
    bind(context, 'j', MDFR_NONE, focus_window_up);
    bind(context, 'j', MDFR_CTRL, focus_window_up);
    bind(context, 'k', MDFR_NONE, focus_window_down);
    bind(context, 'k', MDFR_CTRL, focus_window_down);
    bind(context, 'l', MDFR_NONE, focus_window_right);
    bind(context, 'l', MDFR_CTRL, focus_window_right);
    bind(context, key_esc, MDFR_NONE, enter_normal_mode_on_current);
    end_map(context);

    // Lister UI bindings
    // Have to improvise here because vim had no such thing and it's really weird
    // if you can't just type into it (ironically enough...)
    begin_map(context, default_lister_ui_map);
    bind_vanilla_keys(context, lister__write_character);
    bind(context, key_esc, MDFR_NONE, lister__quit);
    bind(context, '\n', MDFR_NONE, lister__activate);
    bind(context, '\t', MDFR_NONE, lister__activate);
    bind(context, key_back, MDFR_NONE, lister__backspace_text_field);
    bind(context, 'k', MDFR_CTRL, lister__move_up);
    bind(context, key_up, MDFR_CTRL, lister__move_up);
    bind(context, 'j', MDFR_CTRL, lister__move_down);
    bind(context, key_down, MDFR_CTRL, lister__move_down);
    bind(context, 'u', MDFR_CTRL, lister__page_up);
    bind(context, 'd', MDFR_CTRL, lister__page_down);
    bind(context, key_mouse_wheel, MDFR_NONE, lister__wheel_scroll);
    bind(context, key_mouse_left, MDFR_NONE, lister__mouse_press);
    bind(context, key_mouse_left_release, MDFR_NONE, lister__mouse_release);
    bind(context, key_mouse_move, MDFR_NONE, lister__repaint);
    bind(context, key_animate, MDFR_NONE, lister__repaint);
    end_map(context);
}
