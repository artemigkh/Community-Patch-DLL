| mode | runs | frames | host frame time mean (ms) | spread of run means | p99 (ms) | fps | DLL CPU in insertion mean / p99 (ms) | AcquireSync CPU mean (ms) | GPU draw-only mean (ms) | GPU copy+draw mean (ms) |
|---|---|---|---|---|---|---|---|---|---|---|
| nodll | 3 | 556148 | 0.0927 | 0.0821 - 0.1063 | 0.2959 | 10784 | - | - | - | - |
| off | 3 | 549789 | 0.0944 | 0.0819 - 0.1119 | 0.3101 | 10597 | - | - | - | - |
| passthrough | 3 | 550803 | 0.0927 | 0.0893 - 0.0968 | 0.2906 | 10787 | - | - | - | - |
| composite_clear | 3 | 584818 | 0.0873 | 0.0844 - 0.0907 | 0.2892 | 11459 | 0.0053 / 0.0116 | 0.126 | 0.0143 | 0.0777 |
| composite_present | 3 | 582290 | 0.0877 | 0.0848 - 0.0915 | 0.2895 | 11403 | 0.0051 / 0.0103 | 0.130 | 0.0135 | 0.0748 |
