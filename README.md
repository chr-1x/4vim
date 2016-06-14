# 4vim
A Vim Plugin for Allen Webster's [4coder](https://4coder.handmade.network/).

Very much a work-in-progress! Any and all contributions are welcome, including issues, feedback, and pull requests.

## Known bugs
  - Movement by word is incorrect
  - 4coder-native menus (e.g. filesystem browser) use default 4coder-style bindings
  - In certain situations, status bars can build up at the top of the screen

## Overview of missing features
There are a ton of missing features. Here's the big ones:
- Movement-chords appending to chord bar
- Making chord bar show up all the time to avoid jitter
- Missing movements
 - rfind and rtil
  - %
  - Search acting as a movement
  - {, }, (, ) 
  - ^
  - * (search under cursor)
- Format chordmode
- Visual block mode
- most G-started chords
- Most of the window chords
- Multiple marks
- A bunch of statusbar commands
- Macros

You can help by tackling these! Please read through the existing code first to get an idea of how I'm using the 4coder API -- most calls are indirect, going through wrappers in the vim layer to allow for things like movements and modes to work properly.
