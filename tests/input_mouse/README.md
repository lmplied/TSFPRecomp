# Mouse bindings

Windows regression, no window and no game: synthetic raw mouse reports go in
through `recomp_mouse_feed()` and the guest pad comes out of
`recomp_bindings_sample()`, using a binding file written to a temporary
folder. It checks that:

- the mouse reaches nothing until it is captured, and nothing once released;
- motion scales a stick with speed, saturates rather than wraps, centres once
  the mouse is still, and any motion at all clears the title's own deadzone
  (`mouse_deadzone`);
- moving the mouse away from the player is stick up;
- a click whose release arrives in the same report still holds the control
  long enough for a title to see it, and a held button stays held;
- a wheel notch is one short press.

```powershell
cmake -S tests/input_mouse -B build/input-mouse -A x64
cmake --build build/input-mouse --config Release --target input_mouse_test
ctest --test-dir build/input-mouse -C Release --output-on-failure
```
