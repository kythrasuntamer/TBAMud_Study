# MVP iedit notes

`iedit` is an object instance editor.

Confirmed behavior:
- `iedit sword` can edit the short description of one live object.
- Loading another copy of the same object keeps the original prototype description.
- Edited instance survived quit/re-enter/relogin inventory save.
- Dangerous fields such as Values are blocked in the MVP.

Test object:
- VNUM 24712, `a crystal sword`
- Edited instance short desc: `Kythra's Crystal Sword.`
