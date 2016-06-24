/* 4Coder Vim Plugin base.
 * By Andrew "ChronalDragon" Chronister
 *
 * NOTE: 
 *    Requires you to implement a few things in your own _custom!
 *    My personal custom layer 4coder_chronal.cpp is included as 
 *    an example of how to use this. Please take a look at that 
 *    if you are confused.
 */

#include "4coder_default_include.cpp"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Known Bugs:
 *  - Movement by word is incorrect
 *  - 4coder-native menus (e.g. filesystem browser) use default 4coder-style bindings
 *  - In certain situations, status bars can build up at the top of the screen
 *
 * Overview of missing features:
 *  - Movement-chords appending to chord bar
 *  - Making chord bar show up all the time to avoid jitter
 *  - Missing movements
 *      - rfind and rtil
 *      - %
 *      - Search acting as a movement
 *      - {, }, (, )
 *      - ^
 *      - * (search under cursor)
 *  - Format chordmode
 *  - Visual block mode
 *  - G chords
 *  - Rest of the window chords
 *  - Multiple marks
 *  - Shittons of statusbar commands
 *  - Macros
 */

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
    vimaction_yank_range,
    vimaction_format_range,
};

struct Vim_Register {
    String text;
    bool is_line;
};

enum Register_Id {
    reg_unnamed = 0,
    reg_sysclip,
    reg_a, reg_b, reg_c, reg_d, reg_e, reg_f, reg_g, reg_h, reg_i, 
    reg_j, reg_k, reg_l, reg_m, reg_n, reg_o, reg_p, reg_q, reg_r, 
    reg_s, reg_t, reg_u, reg_v, reg_w, reg_x, reg_y, reg_z,
    reg_1, reg_2, reg_3, reg_4, reg_5, reg_6, reg_7, reg_8, reg_9, reg_0 
};

Register_Id regid_from_char(char C) {
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

    if (C == '*') { return reg_sysclip; }

    return reg_unnamed;
}

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

    Vim_Mode mode;
    int cursor;

    Pending_Action action;
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

    //TODO(chronister): Actually there needs to be one of these per file!
    // Until I can use the GUI customization to make my own, anyway.
    bool chord_bar_exists;
    Query_Bar chord_bar;
    char chord_str[10];
    int chord_str_len;
};

/*                             *
 * User-defined function defns *
 *                             */

void on_enter_normal_mode(struct Application_Links *app);
void on_enter_insert_mode(struct Application_Links *app);
void on_enter_replace_mode(struct Application_Links *app);
void on_enter_visual_mode(struct Application_Links *app);

/*                         *
 * (Temp) Global Variables *
 *                         */

static Vim_State state = {};

/*                 *
 * Custom commands *
 *                 */

static void set_current_keymap(struct Application_Links* app, int map) {
    unsigned int access = AccessAll;
    View_Summary view = app->get_active_view(app, access);
    Buffer_Summary buffer = app->get_buffer(app, view.buffer_id, access);
 
#if 0
    if (!buffer.exists) {
        buffer = app->get_parameter_buffer(app, 0, access);
    }
#endif

    if (!buffer.exists) { return; }

#if 0
    push_parameter(app, par_buffer_id, buffer.buffer_id);
    push_parameter(app, par_key_mapid, map);
    exec_command(app, cmdid_set_settings);
#endif
    app->buffer_set_setting(app, &buffer, BufferSetting_MapID, map);
}

static char get_cursor_char(struct Application_Links* app, int offset = 0) {
    Buffer_Summary buffer;
    View_Summary view;
    
    unsigned int access = AccessOpen;
    view = app->get_active_view(app, access);
    buffer = app->get_buffer(app, view.buffer_id, access);
    
    int res;
    char read; 
    res = app->buffer_read_range(app, &buffer, view.cursor.pos + offset, view.cursor.pos + offset + 1, &read);
    if (res) { return read; }
    else { return 0; }
}

static int get_cursor_pos(struct Application_Links* app) {
    View_Summary view;
    
    unsigned int access = AccessAll;
    view = app->get_active_view(app, access);
    return view.cursor.pos;
}

static int get_line_start(struct Application_Links* app, int cursor = -1) {
    unsigned int access = AccessAll;
    View_Summary view = app->get_active_view(app, access);
    Buffer_Summary buffer = app->get_buffer(app, view.buffer_id, access);
    if (cursor == -1) {
        cursor = view.cursor.pos;
    }
    
    int new_pos = seek_line_beginning(app, &buffer, cursor);
    return new_pos;
}

static int get_line_end(struct Application_Links* app, int cursor = -1) {
    unsigned int access = AccessAll;
    View_Summary view = app->get_active_view(app, access);
    Buffer_Summary buffer = app->get_buffer(app, view.buffer_id, access);
    if (cursor == -1) {
        cursor = view.cursor.pos;
    }
    
    int new_pos = seek_line_end(app, &buffer, cursor);
    return new_pos;
}

static void update_visual_range(struct Application_Links* app, int end_new) {
    View_Summary view;
    
    unsigned int access = AccessOpen;
    view = app->get_active_view(app, access);

    state.selection_cursor.end = end_new;
    Range normalized = make_range(state.selection_cursor.start, state.selection_cursor.end);
    state.selection_range = make_range(normalized.start, normalized.end + 1);
    app->view_set_highlight(app, &view, normalized.start, normalized.end + 1, true);
    
    // NOTE(allen): We don't need this, it's basically equivalent to a "get" call.
    // I want to eliminate it somehow.
    //app->refresh_view(app, &view);
}

static void update_visual_line_range(struct Application_Links* app, int end_new) {
    View_Summary view;
    
    unsigned int access = AccessOpen;
    view = app->get_active_view(app, access);

    state.selection_cursor.end = end_new;
    Range normalized = make_range(state.selection_cursor.start, state.selection_cursor.end);
    state.selection_range = make_range(get_line_start(app, normalized.start), 
                                       get_line_end(app, normalized.end) + 1);
    app->view_set_highlight(app, &view, state.selection_range.start, state.selection_range.end, true);
    
    //app->refresh_view(app, &view);
}

static void end_visual_selection(struct Application_Links* app) {
    View_Summary view;
    
    unsigned int access = AccessOpen;
    view = app->get_active_view(app, access);

    state.selection_range.start = state.selection_range.end = -1;
    state.selection_cursor.start = state.selection_cursor.end = -1;
    app->view_set_highlight(app, &view, 0, 0, false);
    
    //app->refresh_view(app, &view);
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

static void push_to_chord_bar(struct Application_Links* app, char* str) {
    if (!state.chord_bar_exists) {
        if (app->start_query_bar(app, &state.chord_bar, 0) == 0) return;
        state.chord_str_len = 0;
        memset(state.chord_str, '\0', ArrayCount(state.chord_str));
        state.chord_bar_exists = true;
    }
    state.chord_str_len = push_to_string(state.chord_str, state.chord_str_len, ArrayCount(state.chord_str),
                                         str, strlen(str));
    state.chord_bar.string = make_string(state.chord_str, state.chord_str_len, ArrayCount(state.chord_str));
}

static void end_chord_bar(struct Application_Links* app) {
    if (state.chord_bar_exists) {
        app->end_query_bar(app, &state.chord_bar, 0);
        state.chord_str_len = 0;
        memset(state.chord_str, '\0', ArrayCount(state.chord_str));
        state.chord_bar_exists = false;
    }
}

CUSTOM_COMMAND_SIG(enter_normal_mode){
    if (state.mode == mode_visual ||
        state.mode == mode_visual_line) {
        end_visual_selection(app);
    }

    state.action = vimaction_none;
    state.mode = mode_normal;
    end_chord_bar(app);

    set_current_keymap(app, mapid_normal);

    on_enter_normal_mode(app);
}

// NOTE(allen): Here's how I got this working for now, since I'm passing the
// buffer_id for hooks directly now.
static void enter_normal_mode(struct Application_Links *app, int buffer_id){
    unsigned int access = AccessAll;
    Buffer_Summary buffer;
    
    if (state.mode == mode_visual ||
        state.mode == mode_visual_line) {
        end_visual_selection(app);
    }

    state.action = vimaction_none;
    state.mode = mode_normal;
    end_chord_bar(app);

    buffer = app->get_buffer(app, buffer_id, access);
    app->buffer_set_setting(app, &buffer, BufferSetting_MapID, mapid_normal);

    on_enter_normal_mode(app);
}

void copy_into_register(struct Application_Links* app, Buffer_Summary* buffer, Range range, Vim_Register* target_register)
{
    free(target_register->text.str);
    //TODO(chronister): Something a little more efficient than malloc?
    target_register->text = make_string((char*)malloc(range.end - range.start), range.end - range.start);
    int res = app->buffer_read_range(app, buffer, range.start, range.end, target_register->text.str);
    res;
}

void vim_exec_action(struct Application_Links* app, Range range)
{
    Buffer_Summary buffer;
    View_Summary view;
    
    unsigned int access = AccessOpen;
    view = app->get_active_view(app, access);
    buffer = app->get_buffer(app, view.buffer_id, access);

    switch (state.action) {
        case vimaction_delete_range: {
            char line_test[2];
            bool is_line = true;
            int res; 
            res = app->buffer_read_range(app, &buffer, range.start-1, range.start, line_test);
            is_line = is_line && (line_test[0] == '\n' || range.start == 0);
            res = app->buffer_read_range(app, &buffer, range.end-1, range.end, line_test);
            is_line = is_line && (line_test[0] == '\n');

            Vim_Register* target_register = state.registers + state.yank_register;
            target_register->is_line = is_line;
            copy_into_register(app, &buffer, range, state.registers + state.yank_register);
            
#if 0
            push_parameter(app, par_range_start, range.start);
            push_parameter(app, par_range_end, range.end);
            exec_command(app, cmdid_cut);
#endif
            clipboard_cut(app, range.start, range.end, 0, access);
        } break;

        case vimaction_yank_range: {
            //TODO(chronister): @copypasta
            char line_test[2];
            bool is_line = true;
            int res; 
            res = app->buffer_read_range(app, &buffer, range.start-1, range.start, line_test);
            is_line = is_line && (line_test[0] == '\n' || range.start == 0);
            res = app->buffer_read_range(app, &buffer, range.end-1, range.end, line_test);
            is_line = is_line && (line_test[0] == '\n');

            Vim_Register* target_register = state.registers + state.yank_register;
            target_register->is_line = is_line;

            copy_into_register(app, &buffer, range, target_register);
        } break;
    }

    switch (state.mode) {
        case mode_normal: {
            exec_command(app, enter_normal_mode);
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

CUSTOM_COMMAND_SIG(enter_insert_mode){
    state.mode = mode_insert;
    set_current_keymap(app, mapid_insert);
    on_enter_insert_mode(app);
}

CUSTOM_COMMAND_SIG(enter_replace_mode){
    state.mode = mode_replace;
    set_current_keymap(app, mapid_replace);
    on_enter_replace_mode(app);
}

CUSTOM_COMMAND_SIG(enter_visual_mode){
    state.mode = mode_visual;
    state.selection_cursor.start = get_cursor_pos(app);
    state.selection_cursor.end = state.selection_cursor.start;
    update_visual_range(app, state.selection_cursor.end);

    set_current_keymap(app, mapid_visual);
    on_enter_visual_mode(app);
}

CUSTOM_COMMAND_SIG(enter_visual_line_mode){
    exec_command(app, enter_visual_mode);
    state.mode = mode_visual_line;
    update_visual_line_range(app, state.selection_cursor.end);
}

CUSTOM_COMMAND_SIG(enter_chord_replace_single){
    set_current_keymap(app, mapid_chord_replace_single);
}

CUSTOM_COMMAND_SIG(enter_chord_switch_registers){
    set_current_keymap(app, mapid_chord_move_til);

    push_to_chord_bar(app, "\"");
}

CUSTOM_COMMAND_SIG(replace_character) {
    //TODO(chronister): Do something a little more intelligent when at the end of a line
    if (get_cursor_char(app) != '\n') {
        exec_command(app, delete_char);
    }
    exec_command(app, write_character);
}

CUSTOM_COMMAND_SIG(replace_character_then_normal) {
    exec_command(app, replace_character);
    exec_command(app, move_left);
    exec_command(app, enter_normal_mode);
}

CUSTOM_COMMAND_SIG(seek_top_of_file) {
    unsigned int access = AccessProtected;
    View_Summary view = app->get_active_view(app, access);
    app->view_set_cursor(app, &view, seek_pos(0), true);
}

CUSTOM_COMMAND_SIG(seek_bottom_of_file) {
    unsigned int access = AccessProtected;
    View_Summary view = app->get_active_view(app, access);
    Buffer_Summary buffer = app->get_buffer(app, view.buffer_id, access);
    app->view_set_cursor(app, &view, seek_pos(buffer.size), true);
}

template <CUSTOM_COMMAND_SIG(command)>
CUSTOM_COMMAND_SIG(compound_move_command){
    View_Summary view;
    int pos1, pos2;
    
    unsigned int access = AccessProtected;
    view = app->get_active_view(app, access);
    
    pos1 = view.cursor.pos;
    exec_command(app, command);
    refresh_view(app, &view);
    pos2 = view.cursor.pos;
    
    vim_exec_action(app, make_range(pos1, pos2));
}

#define vim_move_left compound_move_command<move_left>
#define vim_move_right compound_move_command<move_right>
#define vim_move_end_of_line compound_move_command<seek_end_of_line>
#define vim_move_beginning_of_line compound_move_command<seek_beginning_of_line>
#define vim_move_whitespace_up compound_move_command<seek_whitespace_up>
#define vim_move_whitespace_down compound_move_command<seek_whitespace_down>
#define vim_move_to_top compound_move_command<seek_top_of_file>
#define vim_move_to_bottom compound_move_command<seek_bottom_of_file>

CUSTOM_COMMAND_SIG(move_forward_word_start){
    View_Summary view;
    
    unsigned int access = AccessProtected;
    view = app->get_active_view(app, access);

    int pos1 = view.cursor.pos;
    
#if 0
    push_parameter(app, par_flags, BoundryAlphanumeric);
    exec_command(app, cmdid_seek_right);
#endif
    basic_seek(app, true, BoundryAlphanumeric);
    
    exec_command(app, move_right);
    refresh_view(app, &view);
    int pos2 = view.cursor.pos;

    vim_exec_action(app, make_range(pos1, pos2));
}

CUSTOM_COMMAND_SIG(move_backward_word_start){
    View_Summary view;
    
    unsigned int access = AccessProtected;
    view = app->get_active_view(app, access);

    int pos1 = view.cursor.pos;
    
#if 0
    push_parameter(app, par_flags, BoundryToken | BoundryWhitespace);
    exec_command(app, cmdid_seek_left);
#endif
    basic_seek(app, false, BoundryToken | BoundryWhitespace);
    
    refresh_view(app, &view);
    int pos2 = view.cursor.pos;

    vim_exec_action(app, make_range(pos1, pos2));
}

CUSTOM_COMMAND_SIG(move_forward_word_end){
    View_Summary view;
    
    unsigned int access = AccessOpen;
    view = app->get_active_view(app, access);

    int pos1 = view.cursor.pos;
    exec_command(app, move_right);
    
#if 0
    push_parameter(app, par_flags, BoundryWhitespace);
    exec_command(app, cmdid_seek_right);
#endif
    basic_seek(app, true, BoundryWhitespace);
    
    refresh_view(app, &view);
    int pos2 = view.cursor.pos;
    exec_command(app, move_left);

    vim_exec_action(app, make_range(pos1, pos2));
}

CUSTOM_COMMAND_SIG(newline_then_insert_before){
    exec_command(app, seek_beginning_of_line);
    write_string(app, make_lit_string("\n"));
    exec_command(app, move_left);
    exec_command(app, enter_insert_mode);
}

CUSTOM_COMMAND_SIG(insert_at){
    exec_command(app, enter_insert_mode);
}

CUSTOM_COMMAND_SIG(insert_after){
    exec_command(app, move_right);
    exec_command(app, enter_insert_mode);
}

CUSTOM_COMMAND_SIG(seek_eol_then_insert){
    exec_command(app, seek_end_of_line);
    exec_command(app, enter_insert_mode);
}

CUSTOM_COMMAND_SIG(newline_then_insert_after){
    exec_command(app, seek_end_of_line);
    write_string(app, make_lit_string("\n"));
    exec_command(app, enter_insert_mode);
}

CUSTOM_COMMAND_SIG(enter_chord_delete){
    set_current_keymap(app, mapid_chord_delete);

    state.action = vimaction_delete_range;

    push_to_chord_bar(app, "d");
}

CUSTOM_COMMAND_SIG(enter_chord_yank){
    set_current_keymap(app, mapid_chord_yank);

    state.action = vimaction_yank_range;

    push_to_chord_bar(app, "y");
}

CUSTOM_COMMAND_SIG(enter_chord_window){
    set_current_keymap(app, mapid_chord_window);

    push_to_chord_bar(app, "^W");
}

CUSTOM_COMMAND_SIG(enter_chord_move_find){
    //TODO(chronister): want this represented on the chord bar
    set_current_keymap(app, mapid_chord_move_find);

    push_to_chord_bar(app, "f");
}

CUSTOM_COMMAND_SIG(enter_chord_move_til){
    //TODO(chronister): want this represented on the chord bar
    set_current_keymap(app, mapid_chord_move_til);

    push_to_chord_bar(app, "t");
}

CUSTOM_COMMAND_SIG(enter_chord_g){
    set_current_keymap(app, mapid_chord_g);

    push_to_chord_bar(app, "g");
}

CUSTOM_COMMAND_SIG(move_line_exec_action){
    View_Summary view;
    int pos1, pos2;
    
    unsigned int access = AccessProtected;
    view = app->get_active_view(app, access);

    exec_command(app, seek_beginning_of_line);
    refresh_view(app, &view);
    pos1 = view.cursor.pos;

    exec_command(app, seek_end_of_line);
    refresh_view(app, &view);
    pos2 = view.cursor.pos + 1;
    
    vim_exec_action(app, make_range(pos1, pos2));
}

CUSTOM_COMMAND_SIG(delete_line){
    state.action = vimaction_delete_range;
    exec_command(app, move_line_exec_action);
}

CUSTOM_COMMAND_SIG(yank_line){
    state.action = vimaction_yank_range;
    exec_command(app, move_line_exec_action);
}

CUSTOM_COMMAND_SIG(seek_find_character){
    Buffer_Summary buffer;
    View_Summary view;
    User_Input trigger;
    int pos1, pos2;
    
    unsigned int access = AccessProtected;
    view = app->get_active_view(app, access);
    buffer = app->get_buffer(app, view.buffer_id, access);

    trigger = app->get_command_input(app);

    pos1 = view.cursor.pos;
    buffer_seek_delimiter_forward(app, &buffer, pos1, trigger.key.character, &pos2);
    Buffer_Seek seek;
    seek.type = buffer_seek_pos;
    seek.pos = pos2;
    app->view_set_cursor(app, &view, seek, 1);
    refresh_view(app, &view);
    
    if (pos2 >= 0) {
        vim_exec_action(app, make_range(pos1, pos2));
    }
    else {
        //TODO(chronister): This will not be correct for visual mode!
        exec_command(app, enter_normal_mode);
    }
}

CUSTOM_COMMAND_SIG(seek_til_character){
    Buffer_Summary buffer;
    View_Summary view;
    User_Input trigger;
    int pos1, pos2;
    
    unsigned int access = AccessProtected;
    view = app->get_active_view(app, access);
    buffer = app->get_buffer(app, view.buffer_id, access);

    trigger = app->get_command_input(app);

    pos1 = view.cursor.pos;
    buffer_seek_delimiter_forward(app, &buffer, pos1, trigger.key.character, &pos2);
    Buffer_Seek seek;
    seek.type = buffer_seek_pos;
    seek.pos = pos2;
    app->view_set_cursor(app, &view, seek, 1);
    refresh_view(app, &view);
    
    if (pos2 >= 0) {
        //TODO(chronister): This command is identical to the above except for the + 1
        vim_exec_action(app, make_range(pos1, pos2+1));
    }
    else {
        //TODO(chronister): This will not be correct for visual mode!
        exec_command(app, enter_normal_mode);
    }
}

//TODO(chronister): move_up and move_down both operate on lines, which is not reflected here.
CUSTOM_COMMAND_SIG(vim_move_up){
    View_Summary view;
    int pos1, pos2;
    
    unsigned int access = AccessProtected;
    view = app->get_active_view(app, access);

    pos1 = view.cursor.pos;

    exec_command(app, move_up);
    refresh_view(app, &view);
    pos2 = view.cursor.pos;
    
    vim_exec_action(app, make_range(pos1, pos2));
}

CUSTOM_COMMAND_SIG(vim_move_down){
    View_Summary view;
    int pos1, pos2;
    
    unsigned int access = AccessProtected;
    view = app->get_active_view(app, access);

    pos1 = view.cursor.pos;

    exec_command(app, move_down);
    refresh_view(app, &view);
    pos2 = view.cursor.pos;
    
    vim_exec_action(app, make_range(pos1, pos2));
}

CUSTOM_COMMAND_SIG(cycle_window_focus){
    set_current_keymap(app, mapid_normal);

    end_chord_bar(app);
    exec_command(app, cmdid_change_active_panel);
}

CUSTOM_COMMAND_SIG(open_window_hsplit){
    set_current_keymap(app, mapid_normal);

    end_chord_bar(app);
    exec_command(app, cmdid_open_panel_hsplit);
}

//TODO(chronister): Enumerate through views using get_view_first and get_view_nexct
// to determine if there's only one view left
CUSTOM_COMMAND_SIG(close_window){
    set_current_keymap(app, mapid_normal);

    end_chord_bar(app);
    exec_command(app, cmdid_close_panel);
}

CUSTOM_COMMAND_SIG(combine_with_next_line){
    exec_command(app, seek_end_of_line);
    exec_command(app, delete_char);
}

CUSTOM_COMMAND_SIG(paste_before){ 
    View_Summary view;
    Buffer_Summary buffer;
    int pos1;
    
    unsigned int access = AccessOpen;
    view = app->get_active_view(app, access);
    buffer = app->get_buffer(app, view.buffer_id, access);

    Vim_Register* reg = state.registers + state.paste_register;
    if (reg->is_line) {
        exec_command(app, seek_beginning_of_line);
        refresh_view(app, &view);
    }
    pos1 = view.cursor.pos;
    app->buffer_replace_range(app, &buffer, pos1, pos1, reg->text.str, reg->text.size);
    exec_command(app, move_left);
    exec_command(app, seek_beginning_of_line);
}

CUSTOM_COMMAND_SIG(paste_after){
    View_Summary view;
    Buffer_Summary buffer;
    int pos1;
    
    unsigned int access = AccessOpen;
    view = app->get_active_view(app, access);
    buffer = app->get_buffer(app, view.buffer_id, access);

    Vim_Register* reg = state.registers + state.paste_register;
    if (reg->is_line) {
        exec_command(app, seek_end_of_line);
        exec_command(app, move_right);
        refresh_view(app, &view);
    }
    pos1 = view.cursor.pos;
    app->buffer_replace_range(app, &buffer, pos1, pos1, reg->text.str, reg->text.size);
    exec_command(app, move_left);
    exec_command(app, seek_beginning_of_line);
}

CUSTOM_COMMAND_SIG(visual_delete) {
    state.action = vimaction_delete_range;
    vim_exec_action(app, state.selection_range);
    exec_command(app, enter_normal_mode);
}

CUSTOM_COMMAND_SIG(visual_yank) {
    state.action = vimaction_yank_range;
    vim_exec_action(app, state.selection_range);
    exec_command(app, enter_normal_mode);
}

CUSTOM_COMMAND_SIG(select_register) {
    User_Input trigger;
    trigger = app->get_command_input(app);

    Register_Id regid = regid_from_char(trigger.key.character);
    if (regid == reg_unnamed) {
        exec_command(app, enter_normal_mode);
    }
    // NOTE(chronister): Right now, yank_register is in a union with paste_register
    // so only one needs to be set.
    state.yank_register = regid;
    char str[2] = { (char)trigger.key.character, '\0' };
    push_to_chord_bar(app, str);

    exec_command(app, enter_normal_mode);
}

CUSTOM_COMMAND_SIG(vim_open_file_in_quotes){
    //@copypasta from 4coder_default_include.cpp
    View_Summary view;
    Buffer_Summary buffer;
    char short_file_name[128];
    int pos, start, end, size;
    
    unsigned int access = AccessProtected;
    view = app->get_active_view(app, access);
    buffer = app->get_buffer(app, view.buffer_id, access);
    pos = view.cursor.pos;
    buffer_seek_delimiter_forward(app, &buffer, pos + 1, '"', &end);
    buffer_seek_delimiter_backward(app, &buffer, pos, '"', &start);
    
    ++start;
    size = end - start;

    end_chord_bar(app);
    exec_command(app, enter_normal_mode);
    
    // NOTE(allen): This check is necessary because app->buffer_read_range
    // requires that the output buffer you provide is at least (end - start) bytes long.
    if (size < sizeof(short_file_name)){
        char file_name_[256];
        String file_name = make_fixed_width_string(file_name_);
        
        app->buffer_read_range(app, &buffer, start, end, short_file_name);
        
        copy(&file_name, make_string(buffer.file_name, buffer.file_name_len));
        remove_last_folder(&file_name);
        append(&file_name, make_string(short_file_name, size));
        
#if 0
        push_parameter(app, par_name, expand_str(file_name));
        exec_command(app, cmdid_interactive_open);
#endif
        
        view_open_file(app, &view, expand_str(file_name), false);
    }
}

/*                                   *
 * Statusbar processing and commands *
 *                                   */

#define VIM_COMMAND_FUNC_SIG(n) void n(struct Application_Links *app, String command, String argstr)
typedef VIM_COMMAND_FUNC_SIG(Vim_Command_Func);

struct Vim_Command_Defn {
    String command;
    Vim_Command_Func* func;
};

//TODO(chronister): Make these not be globals and be dynamic and be a hashtable
static Vim_Command_Defn defined_commands[512];
static int defined_command_count = 0;

CUSTOM_COMMAND_SIG(status_command){
    User_Input in;
    Query_Bar bar;

    set_current_keymap(app, mapid_normal);

    if (app->start_query_bar(app, &bar, 0) == 0) return;

    char bar_string_space[256];
    bar.string = make_fixed_width_string(bar_string_space);

    bar.prompt = make_lit_string(":");

    while (1){
        in = app->get_user_input(app, EventOnAnyKey, EventOnEsc | EventOnButton);
        if (in.abort) break;
        if (in.key.keycode == '\n'){
            break;
        }
        else if (in.key.keycode == '\t') {
            // Ignore it for now
            // TODO(chronister): auto completion!
        }
        else if (in.key.character && key_is_unmodified(&in.key)){
            append(&bar.string, in.key.character);
        }
        else if (in.key.keycode == key_back){
            if (bar.string.size > 0){
                --bar.string.size;
            }
        }

        if (match(bar.string, make_lit_string("e "))) {
            exec_command(app, cmdid_interactive_open);
            return;
        }

        //TODO(chronister): TEMP! Want to make this use the already-open files instead.
        if (match(bar.string, make_lit_string("b "))) {
            exec_command(app, cmdid_interactive_open);
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

    int arg_start = command_end;
    while (arg_start < bar.string.size && 
           char_is_whitespace(bar.string.str[arg_start])) {
        ++arg_start;
    }
    String argstr = substr(bar.string, arg_start, bar.string.size - arg_start);

    for (int command_index = 0; command_index < defined_command_count; ++command_index) {
        Vim_Command_Defn defn = defined_commands[command_index];
        if (match_part(defn.command, command)) {
            defn.func(app, command, argstr);
        }
    }
}

void define_command(String command, Vim_Command_Func func) {
    Vim_Command_Defn* defn = defined_commands + defined_command_count++;
    defn->command = command;
    defn->func = func;
}

VIM_COMMAND_FUNC_SIG(write_file) {

    View_Summary view;

    unsigned int access = AccessProtected;
    view = app->get_active_view(app, access);
    if (argstr.str == NULL || argstr.size == 0) {
        exec_command(app, cmdid_save);
    }
    else {
#if 0
        push_parameter(app, par_buffer_id, view.buffer_id);
        push_parameter(app, par_save_update_name, expand_str(argstr));
        exec_command(app, cmdid_save);
#endif
        
        Buffer_Summary buffer = app->get_buffer(app, view.buffer_id, access);
        app->save_buffer(app, &buffer, expand_str(argstr), 0);
    }
}

VIM_COMMAND_FUNC_SIG(edit_file) {
    exec_command(app, cmdid_interactive_open);
}

VIM_COMMAND_FUNC_SIG(new_file) {
    exec_command(app, cmdid_interactive_new);
}

VIM_COMMAND_FUNC_SIG(colorscheme) {
    if (argstr.str && argstr.size > 0) {
        app->change_theme(app, expand_str(argstr));
    }
    // else set bar text (...) to current colorscheme
    else {
        exec_command(app, cmdid_open_color_tweaker);
    }
}

VIM_COMMAND_FUNC_SIG(close_buffer) {
    exec_command(app, cmdid_close_panel);
}

VIM_COMMAND_FUNC_SIG(vertical_split) {
    exec_command(app, cmdid_open_panel_vsplit);
}

VIM_COMMAND_FUNC_SIG(horizontal_split) {
    exec_command(app, cmdid_open_panel_hsplit);
}

// CALL ME
// This function should be called from your 4coder custom init hook
HOOK_SIG(vim_hook_init_func) {
//    exec_command(app, enter_normal_mode);
    return 0;
}

// CALL ME
// This function should be called from your 4coder custom open file hook
OPEN_FILE_HOOK_SIG(vim_hook_open_file_func) {
//    exec_command(app, enter_normal_mode);
    enter_normal_mode(app, buffer_id);
    return 0;
}

// CALL ME
// This function should be called from your 4coder custom new file hook
OPEN_FILE_HOOK_SIG(vim_hook_new_file_func) {
//    exec_command(app, enter_normal_mode);
    enter_normal_mode(app, buffer_id);
    return 0;
}

// CALL ME
// This function should be called from your 4coder custom get bindings hook
void vim_get_bindings(Bind_Helper *context) {

    set_scroll_rule(context, smooth_scroll_rule);

    /*                       *
     * SECTION: Vim commands *
     *                       */

    define_command(make_lit_string("write"), write_file);
    define_command(make_lit_string("quit"), close_buffer);
    define_command(make_lit_string("exit"), close_buffer);
    define_command(make_lit_string("close"), close_buffer);
    define_command(make_lit_string("edit"), edit_file);
    define_command(make_lit_string("new"), new_file);
    define_command(make_lit_string("colorscheme"), colorscheme);
    define_command(make_lit_string("vs"), vertical_split);
    define_command(make_lit_string("vsplit"), vertical_split);
    define_command(make_lit_string("sp"), horizontal_split);
    define_command(make_lit_string("split"), horizontal_split);

    /*                          *
     * SECTION: Vim keybindings *
     *                          */

    /* Movements.
     * They move the cursor around.
     * They're useful in a few different modes, so we have
     * them defined globally for other modes to inherit from.
     */
    begin_map(context, mapid_movements);
    bind_vanilla_keys(context, cmdid_null);

    bind(context, key_esc, MDFR_NONE, enter_normal_mode);

    bind(context, 'h', MDFR_NONE, vim_move_left);
    bind(context, 'j', MDFR_NONE, vim_move_down);
    bind(context, 'k', MDFR_NONE, vim_move_up);
    bind(context, 'l', MDFR_NONE, vim_move_right);

    bind(context, 'w', MDFR_NONE, move_forward_word_start);
    bind(context, 'e', MDFR_NONE, move_forward_word_end);
    bind(context, 'b', MDFR_NONE, move_backward_word_start);

    bind(context, 'f', MDFR_NONE, enter_chord_move_find);
    bind(context, 't', MDFR_NONE, enter_chord_move_til);

    bind(context, '$', MDFR_NONE, vim_move_end_of_line);
    bind(context, '0', MDFR_NONE, vim_move_beginning_of_line);
    bind(context, '{', MDFR_NONE, vim_move_whitespace_up);
    bind(context, '}', MDFR_NONE, vim_move_whitespace_down);

    bind(context, 'G', MDFR_NONE, vim_move_to_bottom);

    // Include status command thingy here so that you can do commands in any non-inserty mode
    bind(context, ':', MDFR_NONE, status_command);
    end_map(context);

    /* Normal mode.
     * aka "It's eating all my input, help!" mode.
     * Shortcuts for navigation, entering various modes,
     * dealing with the editor.
     */
    begin_map(context, mapid_normal);
    inherit_map(context, mapid_movements);

    bind(context, 'J', MDFR_NONE, combine_with_next_line);

    //TODO(chronister): Hitting top/bottom of file if near them
    bind(context, 'u', MDFR_CTRL, cmdid_page_up);
    bind(context, 'd', MDFR_CTRL, cmdid_page_down);

    //TODO (chronister): this doesn't go into register like you want
    bind(context, 'x', MDFR_NONE, delete_char);
    //TODO(chronister): Pasting from registers
    bind(context, 'P', MDFR_NONE, paste_before);
    bind(context, 'p', MDFR_NONE, paste_after);

    bind(context, 'u', MDFR_NONE, cmdid_undo);
    bind(context, 'r', MDFR_CTRL, cmdid_redo);

    //TODO(chronister): Navigation of search using vim shortcuts instead of Up/Down
    bind(context, '/', MDFR_NONE, search);

    bind(context, 'i', MDFR_NONE, insert_at);
    bind(context, 'a', MDFR_NONE, insert_after);
    bind(context, 'A', MDFR_NONE, seek_eol_then_insert);
    bind(context, 'o', MDFR_NONE, newline_then_insert_after);
    bind(context, 'O', MDFR_NONE, newline_then_insert_before);
    bind(context, 'r', MDFR_NONE, enter_chord_replace_single);
    bind(context, 'R', MDFR_NONE, enter_replace_mode);
    bind(context, 'v', MDFR_NONE, enter_visual_mode);
    bind(context, 'V', MDFR_NONE, enter_visual_line_mode);

    //TODO(chronister): Proper alphabetic marks
    bind(context, 'm', MDFR_NONE, set_mark);
    bind(context, '`', MDFR_NONE, cursor_mark_swap);

    bind(context, 'd', MDFR_NONE, enter_chord_delete);
    bind(context, 'y', MDFR_NONE, enter_chord_yank);
    bind(context, 'g', MDFR_NONE, enter_chord_g);
    bind(context, 'w', MDFR_CTRL, enter_chord_window);
    bind(context, 'D', MDFR_NONE, delete_line);
    bind(context, 'Y', MDFR_NONE, yank_line);
    //TODO(chronister): ask allen for impl or figure out how to seek to end of file
    //bind(context, 'G', MDFR_NONE, cmdid_seek_end_of_file);

    end_map(context);

    // TODO(chronister): We have to include this here so that you can do commands
    // in empty views. Pester Allen to make empty views actually in-memory files.
    begin_map(context, mapid_unbound);
    inherit_map(context, mapid_movements);

    bind(context, ':', MDFR_NONE, status_command);

    end_map(context);

    /* Visual mode
     * aka "Selecting stuff" mode
     * A very useful mode!
     */
    begin_map(context, mapid_visual);
    inherit_map(context, mapid_movements);
    bind(context, 'd', MDFR_NONE, visual_delete);
    end_map(context);

    /* Insert mode
     * You type and it goes into the buffer. Nice and simple.
     * Escape to exit.
     */
    begin_map(context, mapid_insert);
    inherit_map(context, mapid_nomap);

    bind_vanilla_keys(context, write_character);
    bind(context, ' ', MDFR_SHIFT, write_character);
    bind(context, key_back, MDFR_NONE, backspace_char);
    bind(context, 'n', MDFR_CTRL, cmdid_word_complete);

    bind(context, key_esc, MDFR_NONE, enter_normal_mode);

    end_map(context);

    /* Replace mode
     * You type and it goes into the buffer. Nice and simple.
     * Escape to exit.
     */
    begin_map(context, mapid_replace);
    inherit_map(context, mapid_nomap);

    bind_vanilla_keys(context, replace_character);
    bind(context, ' ', MDFR_SHIFT, write_character);
    bind(context, key_back, MDFR_NONE, backspace_char);
    bind(context, 'n', MDFR_CTRL, cmdid_word_complete);

    bind(context, key_esc, MDFR_NONE, enter_normal_mode);

    end_map(context);

    /* Chord "modes".
     * They're not really an explicit mode per-say, but the meaning of key presses
     * does change once a chord starts, and is context-dependent.
     */
    
    // Single-char replace mode
    begin_map(context, mapid_chord_replace_single);
    inherit_map(context, mapid_nomap);
    bind_vanilla_keys(context, replace_character_then_normal);
    bind(context, key_esc, MDFR_NONE, enter_normal_mode);
    end_map(context);
    
    // Choosing register for yank/paste chords
    begin_map(context, mapid_chord_choose_register);
    inherit_map(context, mapid_nomap);
    bind_vanilla_keys(context, select_register);
    bind(context, key_esc, MDFR_NONE, enter_normal_mode);
    end_map(context);

    // Move-find chords
    begin_map(context, mapid_chord_move_find);
    inherit_map(context, mapid_nomap);
    bind_vanilla_keys(context, seek_find_character);
    bind(context, key_esc, MDFR_NONE, enter_normal_mode);
    end_map(context);

    // Til-find chords
    begin_map(context, mapid_chord_move_til);
    bind_vanilla_keys(context, seek_til_character);
    inherit_map(context, mapid_nomap);
    bind(context, key_esc, MDFR_NONE, enter_normal_mode);
    end_map(context);

    // Delete+movement chords
    begin_map(context, mapid_chord_delete);
    inherit_map(context, mapid_movements);
    bind(context, 'd', MDFR_NONE, move_line_exec_action);
    end_map(context);

    // yank+movement chords
    begin_map(context, mapid_chord_yank);
    inherit_map(context, mapid_movements);
    bind(context, 'y', MDFR_NONE, move_line_exec_action);
    end_map(context);

    // Map for chords which start with the letter g
    begin_map(context, mapid_chord_g);
    inherit_map(context, mapid_nomap);

    bind(context, 'g', MDFR_NONE, vim_move_to_top);
    bind(context, 'f', MDFR_NONE, vim_open_file_in_quotes);

    //TODO(chronister): Folds!

    bind(context, key_esc, MDFR_NONE, enter_normal_mode);
    end_map(context);

    // Window navigation/manipulation chords
    begin_map(context, mapid_chord_window);
    inherit_map(context, mapid_nomap);

    bind(context, 'w', MDFR_NONE|MDFR_CTRL, cycle_window_focus);
    //TODO(chronister): These
    //bind(context, 'v', MDFR_NONE|MDFR_CTRL, open_window_dup_vsplit);
    //bind(context, 's', MDFR_NONE|MDFR_CTRL, open_window_dup_hsplit);
    bind(context, 'n', MDFR_NONE|MDFR_CTRL, open_window_hsplit);
    bind(context, 'q', MDFR_NONE|MDFR_CTRL, close_window);
    bind(context, key_esc, MDFR_NONE, enter_normal_mode);

    end_map(context);
}
