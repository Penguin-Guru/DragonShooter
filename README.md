This program spawns dragons that fly around on your screen until you click on them (or press 'q').

### Some highlight features include:
- Dragon acceleration and starting positions are randomised.
- Dragons spawn small and gradually grow larger up to a limit.
- Dragons try to avoid the cursor.

### Design overview:

The dragon animation frames are loaded from bitmap images, which I disassembled from a gif using ffmpeg. See [references.txt](assets/references.txt) for attribution and the relevant ffmpeg commands.

If the program fails to detect a transparent root window, it will fall back on a composite overlay window (pseudo-transparency).

Separate threads are run for the event, animation, and spawn loops.

A large portion of the code creates an animated cursor that appears while mouse button 1 is depressed. The cursor image is drawn by the program at run-time, since I wanted to try that.

I did not use shared memory or direct rendering-- this uses the X.11 core protocol. Many of the X.C.B. functions are called with synchronous error handling, which is less efficient but simplifies debugging.

---

## How to use it:

### Libraries required:
- xcb
- xcb-errors
- xcb-keysyms
- xcb-composite
- xcb-image
- xcb-render

Run or read [redo.sh](redo.sh) to compile. That (very simple) script should produce one executable file: "dragon-shooter".

The script compiles for debugging, but **DO NOT DEBUG** without the command-line parameter, `--no-overlay`. If you do somehow find yourself blocked by the overlay, and pressing 'q' does not remove it, you can switch to a different T.T.Y. and kill the debugger process.

---

I put together this project for practice and am posting it here so it's available as a reference for others. All feedback is welcome. There are currently a couple bugs and a lot of other things to clean up. Hopefully some of you will find this project as educational as I have.

