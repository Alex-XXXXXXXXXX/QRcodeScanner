# QRCodeScanner

Windows x64、CPU-only 的工业二维码解码原型。首要码制为 Data Matrix ECC200，保留 QR Code；运行时只依赖 Qt 5、ZXing-C++ 和 OpenCV，不依赖付费 SDK。

## 当前实现

- `DecodeEngine::decode(QImage)` 兼容入口与 `decode(const DecodeRequest&)` 工业入口；
- `DecodeRecipe`：固定 ROI/四边形、码制、预期矩阵尺寸、极性、业务正则和分路径预算；
- `DecodeReport`：安全状态、失败阶段、候选四角、矩阵尺寸、置信度及逐阶段耗时；
- L0 原始灰度快速解码；
- OpenCV 双层灰度金字塔、轮廓候选、L 边/交替时钟边评分、Top-3 ROI 与四角透视校正；
- L1/L2 困难模式、对比度、反锐化和局部阈值候选；
- L3 ECC200 合法尺寸、模块多点采样、相位/区域边界/曲面补偿，再交给 ZXing 纠错；
- 固定 2 工作线程、150 ms deadline、同步译码安全余量、成功取消和候选冲突拒读；同一图像包含两个校验有效但内容不同的码时返回 `Conflict`；
- 只有 ZXing 校验和业务格式都通过才输出，其他情况为 `NoRead`/`Timeout`；
- JSONL 数据集、命令行基准、离线决策树训练、固定 C++ 阈值参数和合成失效边界工具；路径特征包含亮度、对比度、饱和、清晰度、边缘一致性、ROI 比例及上一成功路径；
- `IFrameSource`、本地文件源、工业相机后端接口、PLC 触发双曝光择优和上一帧 ROI 预测器。

## 依赖与构建

- Visual Studio 2022
- Qt 5.14.2 `msvc2017_64`
- ZXing-C++ 3.0.2
- OpenCV 4.6.0 或更高版本（当前本机构建使用用户提供的 4.6.0 Windows 包）

```powershell
.\scripts\fetch_dependencies.ps1
.\scripts\build.ps1
.\build\Release\QRCodeScanner.exe
```

依赖脚本会复用 `third_party` 中已有文件。由于 GitHub 的单文件大小限制，OpenCV Windows 安装包和解压目录不纳入仓库；请先将官方 `opencv-4.14.0-windows.exe` 放入 `third_party`。脚本通过系统 `tar.exe` 解包，不执行未签名的自解压程序。构建脚本自动发现 `OpenCVConfig.cmake` 并将对应 `opencv_world*.dll` 部署到 Release/Debug 目录。

## 数据集与基准

每行一个 JSON 对象，最小推荐字段：

```json
{"image":"images/a.jpg","expectedText":"ABC123","format":"Data Matrix","rows":48,"columns":48,"groundTruthCorners":[[10,20],[210,18],[214,222],[12,224]],"split":"test","batch":"lot-17","sequence":"trigger-42","device":"camera-1","tags":["blur","glare"]}
```

同一批次和连续拍摄序列不能跨 train/validation/test。先验证清单：

```powershell
python .\tools\validate_manifest.py .\datasets\manifest.jsonl --require-ground-truth
```

运行冻结集基准：

```powershell
.\build\Release\QRCodeScannerBenchmark.exe `
  --manifest .\datasets\manifest.jsonl --iterations 1 --budget-ms 150 `
  --output .\benchmark-result.json
```

结果包含首读率、误读率、No Read 率、总耗时 P50/P95、逐阶段 P95、路径命中、候选四角，以及有 `groundTruthCorners` 时的 ROI Recall@0.5/0.7；任何误读使程序返回非零退出码。基准还计算 64 位图像差异哈希，跨 split 的近重复（汉明距离 ≤5）会列入 `nearDuplicateLeakage` 并返回退出码 5。`corners` 表示部署配方中的已知四角，`groundTruthCorners` 仅用于评测，二者不要混用。`expectedText` 为空的 `detect_only` 样本只用于定位回归，不能计入首读率验收。

离线路由训练不进入 C++ 运行时：

```powershell
python .\tools\train_route_selector.py .\benchmark-result.json --output .\route-selector.json
```

训练器仅依赖 Python 标准库，也接受 CSV，并导出可审计的节点、阈值和类别计数；部署前人工复核后将阈值写入 `src/RouteModelParameters.h`，运行时不加载 Python 或模型文件。

厂商黑盒 A/B 使用相同冻结清单、每图一次首读结果。厂商 CSV 字段为 `image,status,text,latencyMs`：

```powershell
python .\tools\compare_vendor_ab.py `
  --ours .\benchmark-result.json `
  --vendor .\vendor-results.csv `
  --output .\vendor-ab.json
```

报告包含成对首读率、误读、P50/P95、双方独占成功数和 McNemar 精确检验；`datasets/vendor-results.example.csv` 是格式示例。

合成透视、失焦、运动模糊、低对比度、眩光和遮挡边界：

```powershell
.\build\Release\QRCodeScannerSyntheticSweep.exe --output-dir .\synthetic-output
```

## 测试

```powershell
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' `
  --test-dir .\build -C Release --output-on-failure
```

`QRCodeScannerSelfTest` 覆盖标准 QR/Data Matrix；`QRCodeScannerPipelineTests` 覆盖方形/矩形 ECC200 尺寸、定位边奇偶模式、模块置信度、标准化/多区域非线性/曲面/透视 L3 网格恢复、OpenCV ROI 召回、固定 ROI、业务格式拒绝、双有效码冲突拒读、本地帧源、PLC 双曝光择优、ROI 跟踪和连续 deadline。

## 现场验收边界

代码实现不等于厂商级验收。声明达到本阶段目标前仍必须提供并冻结不少于 1000 张独立采集且有真实内容的图片，在 i5-7260U 上预热后测试，并用同一批图与现用扫码器黑盒 A/B。目标为冻结集首读率不低于 99.5%、零误读、普通码 P95 不高于 20 ms、困难码 P95 不高于 150 ms。

当前现场图片已永久列入 `datasets/manifest.example.jsonl`，但真实编码内容仍为空，因此只能验证定位、拒读安全和耗时，不能计入首读率。

本机最近一次可复现结果见 [VALIDATION.md](VALIDATION.md)。
