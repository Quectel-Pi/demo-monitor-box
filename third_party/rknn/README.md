# RKNN headers

`rknn_api.h` and `rknn_matmul_api.h` are provided by the Rockchip RKNN SDK
and are not redistributed by this project.

Set `RKNN_INCLUDE_DIR` to the directory containing them when building:

```bash
cmake -S . -B build \
  -DRKNN_INCLUDE_DIR=/path/to/librknn_api/include

RKNN_INCLUDE_DIR=/path/to/librknn_api/include ./tools/build_on_board.sh
```

The CMake build also checks the SDK layout used by Quectel Pi M2:
`RK3576_Linux6.1_BL01_v1.2.0/external/rknpu2/runtime/Linux/librknn_api/include`.
