# Build-time Python modules

Pure-Python packages used only on the build host to run Mesa's code
generators (`ports/mesa/generate.py`). Nothing here is installed into the
Nocturne image.

| Package    | Version | License        | Notes |
|------------|---------|----------------|-------|
| Mako       | 1.3.12  | MIT            | unmodified |
| MarkupSafe | 3.0.3   | BSD-3-Clause   | `_speedups` C extension removed; the bundled `_native` fallback is used |
| PyYAML     | 6.0.3   | MIT            | `_yaml` C extension removed; the pure-Python loader is used |

The license texts are in `licenses/`. The modules are copied from the
published wheels as installed by pip; compiled extension modules and caches
are excluded so the same files work with any Python 3 on the build host.
