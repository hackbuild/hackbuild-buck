# Print files

| file | what it is |
|---|---|
| `buck-plates-3mf.zip` | four plates, one per filament: brown (head, jaw, ears), bone (antlers), black (nose, eyes), walnut (plaque, hooks, pivot pin) |
| `buck-one-colour.3mf` | every part on one 256 x 256 bed for a single filament |
| `buck-stl.zip` | each part as a single body STL, plus `print-card.txt` and `design.json` |
| `print-card.txt` | orientation, fits, hardware, and assembly on one page |
| `design.json` | the BUCK bench settings these files came from; load it in the bench to start from this head |
| `buck-bench.html` | the BUCK bench design tool, one offline HTML file: shape the head, check the fit audit, export plates |
| `source/buck-bench-src.zip` | the bench's source (`build.py` assembles `buck-bench.html`) |
| `buck-open-mouth.png` | render of the finished head |

The print card's wiring section lists an ESP32-S3 SuperMini pinout. This repo's firmware runs on the ESP32-C3 SuperMini, which has the same footprint; wire it as shown in [docs/BUILD.md](../docs/BUILD.md).
