# Third-party attribution

The capture plugin, launcher and recurrent exporter are original MIT-licensed integration code. Model libraries, weights and interface headers remain under the licences listed in [the audit](docs/LICENSE-AUDIT.md).

`tools/benchmark.py` adapts the windowing and nonpersonalized P.835 polynomial calibration published in [Microsoft DNS-Challenge's DNSMOS/dnsmos_local.py](https://github.com/microsoft/DNS-Challenge/blob/591184a9fcb2cbdec02520fed81a32bbbf9d73ff/DNSMOS/dnsmos_local.py). Copyright (c) Microsoft Corporation. The MIT terms reproduced in this repository's [LICENSE](LICENSE) also apply to that adaptation; the source's code licence is [LICENSE-CODE](https://github.com/microsoft/DNS-Challenge/blob/591184a9fcb2cbdec02520fed81a32bbbf9d73ff/LICENSE-CODE).

The downloaded DNSMOS model retains attribution under the repository's root [Creative Commons Attribution 4.0 International](https://creativecommons.org/licenses/by/4.0/) licence. Attribution: Microsoft DNS-Challenge; Chandan K. A. Reddy, Vishak Gopal and Ross Cutler, *DNSMOS P.835: A non-intrusive perceptual objective speech quality metric to evaluate noise suppressors*, ICASSP 2022. [Paper](https://arxiv.org/abs/2110.01763).

Changes: this benchmark uses only the primary SIG/BAK/OVRL model, omits the P.808/mel path and personalized calibration, uses SciPy polyphase resampling instead of librosa, limits inference to one thread and evaluates explicitly delayed/flush-trimmed mono outputs. It provides no Microsoft endorsement or subjective quality claim. The model is not committed to this source tree.

The speech corpus used by the benchmark is LibriSpeech test-clean, provided by OpenSLR under CC-BY-4.0. Attribution: Vassil Panayotov, Guoguo Chen, Daniel Povey and Sanjeev Khudanpur, *LibriSpeech: An ASR corpus based on public domain audio books*, ICASSP 2015. [Source and licence](https://www.openslr.org/12/). Changes: four selected utterances are resampled and mixed with seeded synthetic noise at 0/5 dB SNR. The corpus and noisy/enhanced audio are not committed to this repository.
