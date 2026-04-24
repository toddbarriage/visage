# Cursor extraction tools

Source `.cur` files and extraction script for the Win32 grab/grabbing cursor data
embedded in Visage.

## Files

- `cursors/hand_grab.cur`, `cursors/hand_grabbing.cur` — open hand + closed fist
  cursor images from Chromium (`ui/resources/cursors/`). BSD-3-Clause licensed
  by The Chromium Authors. See `visage_windowing/win32/cursor_data.h` for the
  full attribution notice.

- `extract_cursor.py` — parses the `.cur` files and writes
  `visage_windowing/win32/cursor_data.h` with pre-extracted pixel arrays and
  hotspot coordinates. Run from this directory:
  ```
  python3 extract_cursor.py
  ```

Re-run the script if the source `.cur` files are updated.
