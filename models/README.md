# Model files

The project uses the following detector files by default:

| File | Role | Distribution |
| --- | --- | --- |
| `yolov8s-640-640.rknn` | Default COCO detector | Included |
| `yolov8n-640-640.rknn` | Lower-power COCO detector | Included as an optional alternative |
| `coco_80_labels_list.txt` | COCO class labels | Included |

Generate or replace the detector models with:

```bash
tools/get_yolov8.sh s
tools/get_yolov8.sh n
```

The default configuration points to `/opt/monitor_box/models/yolov8s-640-640.rknn`.
The optional low-power configuration points to `yolov8n-640-640.rknn`.

The pose, fire/smoke and open-vocabulary models are optional and are not part of
the default model set. Their conversion workflows and upstream license notes are
documented in `tools/get_rtmpose.sh`, `tools/fire_model/README.md` and the main
README files.

RKNN model conversion must use `rknn-toolkit2` 2.3.2, matching
`librknnrt.so` on the board. The project license does not replace the upstream
license of a third-party model or training pipeline.
