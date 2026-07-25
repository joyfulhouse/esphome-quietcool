# QuietCool host core tests

Build and run the complete allocation-free C++17 core suite on macOS with:

```sh
make -C tests/cpp test
```

The command uses the system C++ compiler only. It does not invoke ESPHome,
PlatformIO, Arduino, ESP-IDF, a radio, or the network.
