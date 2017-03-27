# 4vim
A Vim Plugin for Allen Webster's [4coder](https://4coder.handmade.network/).

Very much a work-in-progress! Any and all contributions are welcome, including issues, feedback, and pull requests.

Use it by adding `#include "4coder_vim.cpp"` at the top of your own 4coder custom file, and then defining the necessary callback functions and making the necessary hook calls. See `4coder_chronal.cpp` for an example of how it should be used.

## Known bugs
  - 4coder-native menus (e.g. filesystem browser) use default 4coder-style bindings
  - Can't do most actions on null buffers

## Overview of missing features
There are a ton of missing features. Here's the big ones:
- Movement-chords appending to chord bar
- Missing movements
  - % and Search acting as a movement
  - {, }, (, ) 
  - ^
- Format chordmode
- Visual block mode
- most G-started chords
- Most of the window chords
- Multiple marks
- A bunch of statusbar commands
- Macros

You can help by tackling these! Please read through the existing code first to get an idea of how I'm using the 4coder API -- most calls are indirect, going through wrappers in the vim layer to allow for things like movements and modes to work properly.
