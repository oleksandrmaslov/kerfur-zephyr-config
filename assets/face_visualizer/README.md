# Kerfur Face Visualizer

Small local viewer for `assets/face/kerfur_faces.json`.

## Run

```powershell
python assets/face_visualizer/server.py
```

Then open:

```text
http://127.0.0.1:8765/assets/face_visualizer/index.html
```

## Notes

- Click `Reload JSON` after editing `kerfur_faces.json` or the SVG assets.
- The viewer understands `slot_overrides` on expressions and reactions.
- Reactions can avoid expression mixing by using `"compose_mode": "rebase"` plus `"base_expression": "PET_EXPR_*"`.
- Reactions can also set face slots directly, for example `"left_eyeball"`, `"mouth"`, `"indicator"`, or `"overlay"`, instead of only legacy `temporary_*` fields.
- Some slot bases are seeded from the current firmware renderer because the JSON does not expose every placement yet.
