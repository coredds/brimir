# Testing Strategy for Brimir

**Date:** November 24, 2025  
**Phase:** 1 - Development  
**Status:** In Progress

---

## Testing Philosophy

### Core Principles
1. **Test Early, Test Often** - Write tests alongside implementation
2. **Test What Matters** - Focus on critical paths and edge cases
3. **Fast Feedback** - Unit tests should run in <1 second
4. **Realistic Testing** - Integration tests with real Saturn data
5. **Continuous Validation** - Automated testing in CI/CD

### Testing Pyramid
```
       /\
      /  \     E2E Tests (Few)
     /----\    - Full games in RetroArch
    /      \   - Manual testing
   /--------\  
  /          \ Integration Tests (Some)
 /   --------\  - CoreWrapper with Ymir
/______________\ Unit Tests (Many)
                - Individual functions
                - Mock dependencies
```

---

## Test Infrastructure

### Framework Selection: **Catch2**

**Why Catch2?**
- ✅ Modern C++20 compatible
- ✅ Header-only option (easy integration)
- ✅ Excellent BDD-style syntax
- ✅ Great documentation
- ✅ Wide adoption
- ✅ vcpkg support

**Alternatives Considered:**
- Google Test: More heavyweight, older style
- doctest: Similar to Catch2, less mature
- Boost.Test: Too heavy for our needs

### Integration Method
```cmake
# via vcpkg
vcpkg install catch2:x64-windows

# CMakeLists.txt
if(BRIMIR_BUILD_TESTS)
    find_package(Catch2 CONFIG REQUIRED)
    enable_testing()
    add_subdirectory(tests)
endif()
```

---

## Test Coverage Goals

### Phase 1 (Current): Core Functionality
**Target Coverage: 70-80%**

| Component | Target | Priority |
|-----------|--------|----------|
| CoreWrapper | 90% | Critical |
| libretro API | 85% | Critical |
| Video Manager | 75% | High |
| Audio Manager | 75% | High |
| Input Manager | 80% | High |
| State Manager | 70% | Medium |

### Phase 2: Advanced Features
**Target Coverage: 80-85%**

| Component | Target | Priority |
|-----------|--------|----------|
| Save States | 90% | Critical |
| Core Options | 85% | High |
| Memory Cards | 80% | Medium |
| Cheats | 60% | Low |

### External Dependencies
**Target Coverage: 0%** (Ymir is externally tested)

- Ymir core: Assume correct (mature project)
- libretro API: Assume correct (standard)
- Focus on **our integration code**

---

## Test Organization

### Directory Structure
```
tests/
├── CMakeLists.txt           # Test build configuration
├── main.cpp                 # Catch2 main runner
├── unit/                    # Unit tests
│   ├── test_core_wrapper.cpp
│   ├── test_video_manager.cpp
│   ├── test_audio_manager.cpp
│   ├── test_input_manager.cpp
│   └── test_state_manager.cpp
├── integration/             # Integration tests
│   ├── test_load_game.cpp
│   ├── test_run_frame.cpp
│   └── test_save_state.cpp
├── fixtures/                # Test data
│   ├── test_rom.bin        # Minimal test ROM
│   ├── test_bios.bin       # Test BIOS
│   └── test_save.sav       # Test save state
└── mocks/                   # Mock objects
    ├── mock_libretro.hpp   # Mock libretro callbacks
    └── mock_ymir.hpp       # Mock Ymir (if needed)
```

---

## Test Categories

### 1. Unit Tests (Fast, Many)
**Execution Time: <1s total**

Test individual functions in isolation.

#### CoreWrapper Tests
```cpp
TEST_CASE("CoreWrapper initialization", "[core][unit]") {
    CoreWrapper core;
    
    SECTION("Starts uninitialized") {
        REQUIRE_FALSE(core.IsInitialized());
    }
    
    SECTION("Initializes successfully") {
        REQUIRE(core.Initialize());
        REQUIRE(core.IsInitialized());
    }
    
    SECTION("Handles double initialization") {
        REQUIRE(core.Initialize());
        REQUIRE(core.Initialize());  // Should be safe
    }
}

TEST_CASE("CoreWrapper game loading", "[core][unit]") {
    CoreWrapper core;
    core.Initialize();
    
    SECTION("Rejects null path") {
        REQUIRE_FALSE(core.LoadGame(nullptr));
    }
    
    SECTION("Rejects invalid path") {
        REQUIRE_FALSE(core.LoadGame("nonexistent.iso"));
    }
    
    SECTION("Loads valid game") {
        REQUIRE(core.LoadGame("tests/fixtures/test_game.iso"));
        REQUIRE(core.IsGameLoaded());
    }
}
```

#### Video Manager Tests
```cpp
TEST_CASE("Video framebuffer", "[video][unit]") {
    VideoManager video;
    
    SECTION("Default resolution is 320x224") {
        REQUIRE(video.GetWidth() == 320);
        REQUIRE(video.GetHeight() == 224);
    }
    
    SECTION("Framebuffer is non-null after init") {
        video.Initialize();
        REQUIRE(video.GetFramebuffer() != nullptr);
    }
    
    SECTION("Pixel format is RGB565") {
        REQUIRE(video.GetPixelFormat() == RETRO_PIXEL_FORMAT_RGB565);
    }
}
```

#### Audio Manager Tests
```cpp
TEST_CASE("Audio buffering", "[audio][unit]") {
    AudioManager audio;
    audio.Initialize();
    
    SECTION("Sample rate is 44.1kHz") {
        REQUIRE(audio.GetSampleRate() == 44100);
    }
    
    SECTION("Buffer size is reasonable") {
        auto size = audio.GetBufferSize();
        REQUIRE(size > 0);
        REQUIRE(size < 8192);  // Not too large
    }
    
    SECTION("Handles buffer underrun") {
        audio.GetSamples();  // Empty buffer
        REQUIRE_NOTHROW(audio.GetSamples());  // Should not crash
    }
}
```

#### Input Manager Tests
```cpp
TEST_CASE("Input mapping", "[input][unit]") {
    InputManager input;
    
    SECTION("Maps libretro buttons to Saturn") {
        auto saturn_a = input.MapButton(RETRO_DEVICE_ID_JOYPAD_A);
        REQUIRE(saturn_a == SaturnButton::A);
    }
    
    SECTION("Handles unmapped buttons") {
        auto result = input.MapButton(999);  // Invalid
        REQUIRE(result == SaturnButton::None);
    }
}
```

---

### 2. Integration Tests (Medium, Some)
**Execution Time: 5-10s total**

Test components working together.

```cpp
TEST_CASE("Load and run game", "[integration]") {
    CoreWrapper core;
    core.Initialize();
    
    REQUIRE(core.LoadGame("tests/fixtures/test_game.iso"));
    
    SECTION("First frame runs without crash") {
        REQUIRE_NOTHROW(core.RunFrame());
    }
    
    SECTION("Video output is generated") {
        core.RunFrame();
        auto fb = core.GetFramebuffer();
        REQUIRE(fb != nullptr);
        // Check for non-zero pixels
        bool has_content = false;
        for (size_t i = 0; i < 320 * 224; ++i) {
            if (fb[i] != 0) {
                has_content = true;
                break;
            }
        }
        REQUIRE(has_content);
    }
    
    SECTION("Audio samples are generated") {
        core.RunFrame();
        auto samples = core.GetAudioSamples();
        auto count = core.GetAudioSampleCount();
        REQUIRE(count > 0);
        REQUIRE(samples != nullptr);
    }
}

TEST_CASE("Save and load state", "[integration][state]") {
    CoreWrapper core;
    core.Initialize();
    core.LoadGame("tests/fixtures/test_game.iso");
    
    // Run a few frames
    for (int i = 0; i < 60; ++i) {
        core.RunFrame();
    }
    
    // Save state
    std::vector<uint8_t> state(core.GetStateSize());
    REQUIRE(core.SaveState(state.data()));
    
    // Run more frames (change state)
    for (int i = 0; i < 60; ++i) {
        core.RunFrame();
    }
    
    // Load state (should restore)
    REQUIRE(core.LoadState(state.data()));
    
    // Verify state was restored
    // (This would need more detailed verification)
}
```

---

### 3. End-to-End Tests (Slow, Few)
**Execution Time: Manual**

Manual testing in real RetroArch environment.

#### Test Cases
1. **Basic Loading**
   - Load core in RetroArch
   - Load commercial game
   - Verify game boots

2. **Video Output**
   - Verify correct resolution
   - Check for tearing/artifacts
   - Test resolution changes

3. **Audio Output**
   - Verify audio plays
   - Check for pops/clicks
   - Test volume control

4. **Input**
   - Test all buttons
   - Test multiple controllers
   - Test input latency

5. **Save States**
   - Create save state
   - Load save state
   - Verify correct restoration

6. **Performance**
   - Check frame rate (60 FPS target)
   - Monitor CPU usage
   - Check for memory leaks

---

## Mock Objects

### Mock libretro Callbacks
```cpp
class MockLibretro {
public:
    std::vector<uint16_t> last_framebuffer;
    std::vector<int16_t> last_audio;
    
    void VideoRefresh(const void* data, unsigned width, unsigned height, size_t pitch) {
        size_t pixels = width * height;
        last_framebuffer.assign(
            static_cast<const uint16_t*>(data),
            static_cast<const uint16_t*>(data) + pixels
        );
    }
    
    void AudioBatch(const int16_t* data, size_t frames) {
        last_audio.assign(data, data + frames * 2);  // Stereo
    }
};
```

---

## Test Data

### Minimal Test ROM
Create a minimal Saturn homebrew ROM that:
- Boots successfully
- Displays simple graphics
- Produces basic audio
- Responds to input
- Size: <100KB

### Test BIOS
- Use a known-good Saturn BIOS dump
- Or create minimal stub for testing
- Store separately (not in repo due to copyright)

---

## Continuous Integration

### GitHub Actions Workflow
```yaml
name: Build and Test

on: [push, pull_request]

jobs:
  test:
    runs-on: windows-latest
    
    steps:
    - uses: actions/checkout@v3
      with:
        submodules: recursive
    
    - name: Setup vcpkg
      run: |
        .\vcpkg\bootstrap-vcpkg.bat
        .\vcpkg\vcpkg install fmt:x64-windows catch2:x64-windows
    
    - name: Configure
      run: |
        cmake -B build -G "Visual Studio 17 2022" -A x64 `
          -DCMAKE_TOOLCHAIN_FILE=vcpkg/scripts/buildsystems/vcpkg.cmake `
          -DBRIMIR_BUILD_TESTS=ON
    
    - name: Build
      run: cmake --build build --config Release
    
    - name: Test
      run: ctest --test-dir build --config Release --output-on-failure
    
    - name: Upload artifacts
      uses: actions/upload-artifact@v3
      with:
        name: brimir-core
        path: build/bin/Release/brimir_libretro.dll
```

---

## Code Coverage

### Tools
- **OpenCppCoverage** (Windows)
- **Codecov** (Web dashboard)

### Target Metrics
- Line Coverage: >75%
- Branch Coverage: >65%
- Function Coverage: >85%

### Excluded from Coverage
- External libraries (Ymir, vendors)
- Test code itself
- Platform-specific workarounds

---

## Performance Testing

### Benchmarks
Using Catch2's `BENCHMARK` macro:

```cpp
TEST_CASE("Frame rendering performance", "[benchmark]") {
    CoreWrapper core;
    core.Initialize();
    core.LoadGame("tests/fixtures/test_game.iso");
    
    BENCHMARK("Single frame") {
        return core.RunFrame();
    };
    
    BENCHMARK("60 frames") {
        for (int i = 0; i < 60; ++i) {
            core.RunFrame();
        }
    };
}
```

### Performance Targets
- Single frame: <16.67ms (60 FPS)
- 60 frames: <1 second
- Memory usage: <500MB
- No memory leaks after 1000 frames

---

## Regression Testing

### Test Retention
- **Never delete tests** unless feature is removed
- Failed tests → fixed code or documented limitation
- All bugs get a regression test

### Test Games Library
Maintain a collection of:
- Commercial games (various regions)
- Homebrew ROMs
- Known problematic games
- Edge cases

---

## Implementation Plan

### Week 3 (This Week)
- [x] Warning analysis
- [ ] Fix our code warnings
- [ ] Set up Catch2
- [ ] Write CoreWrapper unit tests
- [ ] Write Video/Audio/Input unit tests
- [ ] Basic integration test

### Week 4
- [ ] Comprehensive integration tests
- [ ] Performance benchmarks
- [ ] Test fixture creation
- [ ] CI/CD setup

### Phase 2
- [ ] Regression test suite
- [ ] Performance monitoring
- [ ] Code coverage reporting
- [ ] Automated game testing

---

## Test Execution

### Local Development
```powershell
# Build with tests
cmake -B build -DBRIMIR_BUILD_TESTS=ON
cmake --build build --config Release

# Run all tests
ctest --test-dir build --config Release

# Run specific test
build\bin\Release\brimir_tests.exe "[core]"

# Run with verbose output
build\bin\Release\brimir_tests.exe --success
```

### Coverage Report
```powershell
# With OpenCppCoverage
OpenCppCoverage --sources src `
  --export_type html:coverage `
  -- build\bin\Release\brimir_tests.exe
```

---

## Documentation

### Test Documentation Requirements
- [ ] README in tests/ explaining structure
- [ ] Comments explaining complex test logic
- [ ] Examples of adding new tests
- [ ] Coverage report interpretation guide

---

## Review and Maintenance

### Regular Activities
- **Weekly:** Review test failures
- **Monthly:** Update test cases
- **Per Release:** Full regression test
- **Quarterly:** Coverage analysis

---

## Success Criteria

### Definition of "Well Tested"
- ✅ All public APIs have unit tests
- ✅ Critical paths have integration tests
- ✅ No untested bug fixes
- ✅ >75% code coverage
- ✅ All tests pass in CI
- ✅ Performance benchmarks meet targets

---

## Risk Mitigation

### Testing Challenges
1. **Saturn Hardware Complexity**
   - Solution: Focus on integration layer, trust Ymir
   
2. **No Test ROMs**
   - Solution: Create minimal homebrew, use demos
   
3. **Time Constraints**
   - Solution: Prioritize critical paths
   
4. **Difficult to Mock**
   - Solution: Use real Ymir, test at integration level

---

## Conclusion

This testing strategy balances:
- ✅ Comprehensive coverage of our code
- ✅ Fast feedback for developers
- ✅ Realistic integration scenarios
- ✅ Pragmatic approach (trust external libs)
- ✅ Continuous validation

**Next Step:** Implement basic test infrastructure (Week 3)

---

*"Untested code is broken code."* - Ancient Programmer Proverb

