//=============================================================================
// >>> my vim-based 4coder custom <<<
// author: chr <chr@chronal.net>
//
// Sample usage of vim functions, from my own 4coder custom. 
// Feel free to copy and tweak as you like!
//=============================================================================

#include "4coder_vim.cpp"

// These colors are tuned to work with the Dragonfire color scheme.
// TODO(chr): How to best make this configurable?
constexpr int_color color_margin_normal = 0xFF341313;
constexpr int_color color_margin_insert = 0xFF5a3619;
constexpr int_color color_margin_replace = 0xFF5a192e;
constexpr int_color color_margin_visual = 0xFF722b04;

START_HOOK_SIG(chronal_init){
    exec_command(app, open_panel_vsplit);
    exec_command(app, change_active_panel);

    default_4coder_initialize(app);
    // NOTE(chr): Be sure to call the vim custom's hook!
    vim_hook_init_func(app);

    //change_theme(app, literal("Dragonfire"));
    //change_theme(app, literal("Monokai"));

    // no meaning for return
    return 0;
}

OPEN_FILE_HOOK_SIG(chronal_file_settings){
    unsigned int access = AccessAll;
    Buffer_Summary buffer = get_buffer(app, buffer_id, access);

    int treat_as_code = 0;

    if (buffer.file_name && buffer.size < (16 << 20)){
        String ext = file_extension(make_string(buffer.file_name, buffer.file_name_len));
        if (match(ext, make_lit_string("cpp"))) treat_as_code = 1;
        else if (match(ext, make_lit_string("h"))) treat_as_code = 1;
        else if (match(ext, make_lit_string("c"))) treat_as_code = 1;
        else if (match(ext, make_lit_string("hpp"))) treat_as_code = 1;
    }
    
#if 0
    push_parameter(app, par_buffer_id, buffer.buffer_id);
    push_parameter(app, par_lex_as_cpp_file, treat_as_code);
    push_parameter(app, par_wrap_lines, !treat_as_code);
    exec_command(app, cmdid_set_settings);
#endif
    
    buffer_set_setting(app, &buffer, BufferSetting_Lex, treat_as_code);
    buffer_set_setting(app, &buffer, BufferSetting_WrapLine, !treat_as_code);
    
    enter_normal_mode(app, buffer_id);

    // NOTE(chr): Be sure to call the vim custom's hook!
    vim_hook_open_file_func(app, buffer_id);

    return 0;
}

OPEN_FILE_HOOK_SIG(chronal_new_file){
    // NOTE(chr): Be sure to call the vim custom's hook!
    return vim_hook_new_file_func(app, buffer_id);
}

RENDER_CALLER_SIG(default_render_caller){
    vim_render_caller();
    default_render_caller();
}

// NOTE(chr): Define the four functions that the vim plugin wants in order
// to determine what to do when modes change.
// TODO(chr): 
void on_enter_insert_mode(struct Application_Links *app) {
    Theme_Color colors[] = {
        { Stag_Bar_Active, color_margin_insert },
        { Stag_Margin_Active, color_margin_insert },
    };
    set_theme_colors(app, colors, ArrayCount(colors));
}

void on_enter_replace_mode(struct Application_Links *app) {
    Theme_Color colors[] = {
        { Stag_Bar_Active, color_margin_replace },
        { Stag_Margin_Active, color_margin_replace },
    };
    set_theme_colors(app, colors, ArrayCount(colors));
}

void on_enter_normal_mode(struct Application_Links *app) {
    Theme_Color colors[] = {
        { Stag_Bar_Active, color_margin_normal },
        { Stag_Margin_Active, color_margin_normal },
    };
    set_theme_colors(app, colors, ArrayCount(colors));
}

void on_enter_visual_mode(struct Application_Links *app) {
    Theme_Color colors[] = {
        { Stag_Bar_Active, color_margin_visual },
        { Stag_Margin_Active, color_margin_visual },
    };
    set_theme_colors(app, colors, ArrayCount(colors));
}

void chronal_get_bindings(Bind_Helper *context) {
    // Set the hooks
    set_start_hook(context, chronal_init);
    set_open_file_hook(context, chronal_file_settings);
    set_new_file_hook(context, chronal_new_file);
    set_render_caller(context, chronal_render_caller);

    // Call to set the vim bindings
    vim_get_bindings(context);

    // Since keymaps are re-entrant, I can define my own keybindings below
    // here that will apply in the appropriate map:

    begin_map(context, mapid_movements);
    // For example, I forget to hit shift a lot when typing commands. Since
    // semicolon doesn't do much useful in vim by default, I bind it to the
    // same command that colon itself does.
    bind(context, ';', MDFR_NONE, status_command);
    end_map(context);

    // I can also define custom commands very simply:

    // As an example, suppose we want to be able to use 'save' to write the
    // current file:
    define_command(make_lit_string("save"), write_file);
    // (In regular vim, :saveas is a valid command, but this hasn't yet
    // been defined in the 4coder vim layer. If it were, this definition 
    // would be pointless, as :save would match as a substring of :saveas 
    // first.)

    // TODO(chr): Make the statusbar commands more intelligent
    //  so that this isn't an issue.
}

extern "C" int
get_bindings(void *data, int size) {
    Bind_Helper context_ = begin_bind_helper(data, size);
    Bind_Helper *context = &context_;
    
    chronal_get_bindings(context);
    
    int result = end_bind_helper(context);
    return(result);
}
