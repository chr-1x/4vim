//=============================================================================
// >>> my vim-based 4coder custom <<<
// author: chr <chr@chronal.net>
//
// Sample usage of vim functions, from my own 4coder custom. 
// Feel free to copy and tweak as you like!
//=============================================================================

#include "4coder_vim.cpp"

// These colors are tuned to work with the Dragonfire color scheme.
// TODO(chr): How to best make this configurable? Can we query for arbitrary
// variables in the theme?
constexpr int_color color_margin_normal = 0xFF341313;
constexpr int_color color_margin_insert = 0xFF5a3619;
constexpr int_color color_margin_replace = 0xFF5a192e;
constexpr int_color color_margin_visual = 0xFF722b04;

START_HOOK_SIG(chronal_init) {
    default_4coder_initialize(app);
    // NOTE(chr): Be sure to call the vim custom's hook!
    vim_hook_init_func(app, files, file_count, flags, flag_count);
    return(0);
}

// NOTE(chr): Define the four functions that the vim plugin wants in order
// to determine what to do when modes change.
// TODO(chr): 
ENTER_MODE_HOOK_SIG(on_enter_insert_mode) {
    Theme_Color colors[] = {
        { Stag_Bar_Active, color_margin_insert },
        { Stag_Margin_Active, color_margin_insert },
    };
    set_theme_colors(app, colors, ArrayCount(colors));
}

ENTER_MODE_HOOK_SIG(on_enter_replace_mode) {
    Theme_Color colors[] = {
        { Stag_Bar_Active, color_margin_replace },
        { Stag_Margin_Active, color_margin_replace },
    };
    set_theme_colors(app, colors, ArrayCount(colors));
}

ENTER_MODE_HOOK_SIG(on_enter_normal_mode) {
    Theme_Color colors[] = {
        { Stag_Bar_Active, color_margin_normal },
        { Stag_Margin_Active, color_margin_normal },
    };
    set_theme_colors(app, colors, ArrayCount(colors));
}

ENTER_MODE_HOOK_SIG(on_enter_visual_mode) {
    Theme_Color colors[] = {
        { Stag_Bar_Active, color_margin_visual },
        { Stag_Margin_Active, color_margin_visual },
    };
    set_theme_colors(app, colors, ArrayCount(colors));
}

void chronal_get_bindings(Bind_Helper *context) {
    set_all_default_hooks(context);
#if defined(__APPLE__) && defined(__MACH__)
    mac_default_keys(context);
#else
    default_keys(context);
#endif

    // Set the hooks
    set_start_hook(context, chronal_init);
    set_open_file_hook(context, vim_hook_open_file_func);
    set_new_file_hook(context, vim_hook_new_file_func);
    set_render_caller(context, vim_render_caller);

    // vim mode hooks
    vim_mode_hooks.on_enter_normal_mode  = on_enter_normal_mode;
    vim_mode_hooks.on_enter_insert_mode  = on_enter_insert_mode;
    vim_mode_hooks.on_enter_replace_mode = on_enter_replace_mode;
    vim_mode_hooks.on_enter_visual_mode  = on_enter_visual_mode;
    // vim_mode_hooks.on_enter_delete_chord = ;

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
    define_command(make_lit_string("W"), write_file);
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
