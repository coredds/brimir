# Brimir Architecture Overview

This document provides a high-level overview of the Brimir project architecture and how it integrates the Ymir emulator with the libretro API.

## Project Structure

```
brimir/
├── .github/
│   └── workflows/           # CI/CD pipelines
│       ├── build.yml
│       ├── test.yml
│       └── release.yml
│
├── cmake/                   # CMake modules and helpers
│   ├── FindLibretro.cmake
│   └── BrimirConfig.cmake
│
├── docs/                    # Documentation
│   ├── api/                 # API documentation
│   ├── guides/              # User guides
│   └── development/         # Developer documentation
│
├── include/
│   └── brimir/
│       ├── core_wrapper.hpp
│       ├── input_manager.hpp
│       ├── audio_manager.hpp
│       ├── video_manager.hpp
│       └── state_manager.hpp
│
├── resources/
│   ├── info/
│   │   └── brimir_libretro.info    # Core info file
│   └── icons/
│       └── brimir.png
│
├── src/
│   ├── libretro/                    # Libretro API implementation
│   │   ├── libretro.cpp             # Main entry point (retro_* functions)
│   │   ├── callbacks.cpp            # Libretro callbacks setup
│   │   ├── environment.cpp          # Environment callback handling
│   │   └── options.cpp              # Core options definition and handling
│   │
│   ├── bridge/                      # Bridge layer between libretro and Ymir
│   │   ├── core_wrapper.cpp         # Wraps Ymir's emulator core
│   │   ├── input_manager.cpp        # Maps libretro input to Saturn controllers
│   │   ├── audio_manager.cpp        # Handles audio output to libretro
│   │   ├── video_manager.cpp        # Handles video output to libretro
│   │   ├── state_manager.cpp        # Save state serialization
│   │   ├── file_loader.cpp          # File loading via libretro VFS
│   │   └── memory_manager.cpp       # Memory access for cheats/inspection
│   │
│   └── ymir/                        # Ymir emulator (git submodule)
│       └── [Ymir source tree]
│
├── tests/
│   ├── unit/                        # Unit tests
│   ├── integration/                 # Integration tests
│   └── roms/                        # Test ROMs (not committed)
│
├── vcpkg/                           # vcpkg submodule (for dependencies)
│
├── .gitignore
├── .gitmodules                      # Git submodules (Ymir, vcpkg)
├── CMakeLists.txt                   # Main CMake configuration
├── CMakePresets.json                # CMake presets (if present)
├── CONTRIBUTING.md                  # Contribution guidelines
├── LICENSE                          # GPL-3.0
├── Makefile.libretro                # Libretro build bot compatibility
└── README.md                        # Project overview
```

## Architecture Layers

### Layer 1: Libretro API Surface

**Purpose:** Implements the standard libretro core interface

**Components:**
- `libretro.cpp`: Implements all required `retro_*` functions
- `callbacks.cpp`: Manages libretro callbacks (video, audio, input, etc.)
- `environment.cpp`: Handles environment queries and commands
- `options.cpp`: Defines and manages core options

**Key Functions:**
```cpp
// Core lifecycle
void retro_init(void);
void retro_deinit(void);
unsigned retro_api_version(void);
void retro_get_system_info(struct retro_system_info *info);
void retro_get_system_av_info(struct retro_system_av_info *info);

// Game lifecycle
bool retro_load_game(const struct retro_game_info *game);
void retro_unload_game(void);
void retro_run(void);
void retro_reset(void);

// Save states
size_t retro_serialize_size(void);
bool retro_serialize(void *data, size_t size);
bool retro_unserialize(const void *data, size_t size);

// Memory access
void *retro_get_memory_data(unsigned id);
size_t retro_get_memory_size(unsigned id);
```

### Layer 2: Bridge Layer

**Purpose:** Translates between libretro and Ymir APIs

**Components:**

#### Core Wrapper (`core_wrapper.cpp`)
- Encapsulates Ymir's emulator instance
- Manages emulator lifecycle
- Coordinates frame execution
- Handles timing and synchronization

```cpp
class CoreWrapper {
public:
    bool Initialize();
    void Shutdown();
    bool LoadGame(const char* path);
    void UnloadGame();
    void Run();
    void Reset();
    
    // State management
    size_t GetStateSize() const;
    bool SaveState(void* data, size_t size);
    bool LoadState(const void* data, size_t size);
};
```

#### Input Manager (`input_manager.cpp`)
- Maps libretro input to Saturn controller state
- Supports multiple controller types
- Handles input device switching

```cpp
class InputManager {
public:
    void Update();
    void SetControllerType(int port, ControllerType type);
    void MapButton(int port, int libretro_id, int saturn_id);
    SaturnControllerState GetControllerState(int port);
};
```

#### Audio Manager (`audio_manager.cpp`)
- Buffers audio from Ymir
- Delivers to libretro audio callback
- Handles sample rate conversion if needed

```cpp
class AudioManager {
public:
    void Initialize(int sample_rate);
    void PushSamples(const int16_t* samples, size_t count);
    void DeliverToLibretro();
};
```

#### Video Manager (`video_manager.cpp`)
- Converts Ymir's video output to libretro framebuffer
- Handles pixel format conversion
- Manages resolution changes
- Optionally handles hardware rendering

```cpp
class VideoManager {
public:
    void Initialize();
    void SetPixelFormat(retro_pixel_format format);
    void UpdateFramebuffer(const YmirFrameBuffer& frame);
    void DeliverToLibretro();
    void HandleResolutionChange(int width, int height);
};
```

#### State Manager (`state_manager.cpp`)
- Serializes/deserializes Ymir's state
- Ensures forward compatibility
- Handles versioning

```cpp
class StateManager {
public:
    size_t CalculateSize() const;
    bool Serialize(void* data, size_t size);
    bool Deserialize(const void* data, size_t size);
    uint32_t GetVersion() const;
};
```

#### File Loader (`file_loader.cpp`)
- Uses libretro VFS API when available
- Falls back to standard file I/O
- Handles BIOS and game loading

```cpp
class FileLoader {
public:
    bool LoadBIOS(const char* path);
    bool LoadGame(const char* path);
    bool LoadFromVFS(retro_vfs_file_handle* handle);
};
```

### Layer 3: Ymir Emulator Core

**Purpose:** The actual Saturn emulation

**Integration Strategy:**
- Use Ymir as a git submodule
- Minimal modifications to Ymir code
- Create adapter interfaces where needed

**Key Ymir Components Used:**
- CPU emulation (SH-2)
- Graphics (VDP1, VDP2)
- Audio (SCSP)
- CD-ROM subsystem
- Memory management
- Cartridge support

## Data Flow

### Game Execution Flow

```
Frontend (RetroArch)
        │
        ├─→ retro_run()
        │       │
        │       ├─→ CoreWrapper::Run()
        │       │       │
        │       │       ├─→ Ymir::RunFrame()
        │       │       │       │
        │       │       │       ├─→ CPU execution
        │       │       │       ├─→ Graphics rendering
        │       │       │       └─→ Audio generation
        │       │       │
        │       │       ├─→ VideoManager::UpdateFramebuffer()
        │       │       ├─→ AudioManager::PushSamples()
        │       │       └─→ InputManager::Update()
        │       │
        │       ├─→ video_cb(framebuffer)
        │       ├─→ audio_batch_cb(samples)
        │       └─→ input_poll_cb()
        │
        └─→ Frontend displays frame
```

### Input Flow

```
User Input
    │
    ├─→ Frontend captures input
    │
    ├─→ retro_run() calls input_poll_cb()
    │
    ├─→ InputManager::Update()
    │       │
    │       ├─→ Query libretro input states
    │       ├─→ Map to Saturn controller format
    │       └─→ Update Ymir's input state
    │
    └─→ Ymir reads controller state
```

### Save State Flow

```
Save State Request
    │
    ├─→ retro_serialize_size()
    │       └─→ StateManager::CalculateSize()
    │
    ├─→ retro_serialize(buffer)
    │       │
    │       └─→ StateManager::Serialize()
    │               │
    │               ├─→ Write version header
    │               ├─→ Serialize Ymir state
    │               └─→ Write checksums
    │
    └─→ Frontend saves to disk


Load State Request
    │
    ├─→ retro_unserialize(buffer)
    │       │
    │       └─→ StateManager::Deserialize()
    │               │
    │               ├─→ Verify version/checksums
    │               ├─→ Deserialize to Ymir
    │               └─→ Reset frame state
    │
    └─→ Emulation continues
```

## Threading Model

### Libretro Threading Constraints
- Libretro cores are **single-threaded** from the API perspective
- `retro_run()` is called sequentially by the frontend
- Cores must not block for extended periods

### Ymir Threading
- Ymir may use internal threading for optimization
- Must be coordinated with libretro's single-threaded model
- Synchronization points at API boundaries

### Implementation Strategy
```cpp
// In CoreWrapper::Run()
void CoreWrapper::Run() {
    // Poll input at frame start
    m_inputManager.Update();
    
    // Run Ymir for one frame
    // (Ymir may use threads internally, but synchronizes here)
    m_ymirCore.RunFrame();
    
    // Output audio/video
    m_videoManager.DeliverToLibretro();
    m_audioManager.DeliverToLibretro();
}
```

## Configuration Management

### Configuration Sources
1. **Core Options**: Exposed to frontend, user-configurable
2. **Environment Queries**: Frontend capabilities and preferences
3. **Internal Config**: Non-exposed settings, stored in profile directory

### Core Options Example
```cpp
// In options.cpp
static const struct retro_core_option_definition options[] = {
    {
        "brimir_region",
        "Region",
        "System region (auto-detect from disc if Auto)",
        {
            { "auto", "Auto" },
            { "jp", "Japan" },
            { "us", "USA" },
            { "eu", "Europe" },
            { NULL, NULL },
        },
        "auto"
    },
    {
        "brimir_cart_type",
        "Cartridge Type",
        "Expansion cartridge",
        {
            { "auto", "Auto" },
            { "none", "None" },
            { "backup_ram", "Backup RAM (512KB)" },
            { "dram", "DRAM (1MB/4MB)" },
            { "rom", "ROM" },
            { NULL, NULL },
        },
        "auto"
    },
    // ... more options
};
```

### Environment Queries
```cpp
// Query frontend capabilities
bool can_dupe = false;
environ_cb(RETRO_ENVIRONMENT_GET_CAN_DUPE, &can_dupe);

// Set performance level hint
unsigned level = 10;  // Saturn emulation is demanding
environ_cb(RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL, &level);

// Query VFS interface
retro_vfs_interface_info vfs_info = { 3, NULL };
if (environ_cb(RETRO_ENVIRONMENT_GET_VFS_INTERFACE, &vfs_info)) {
    // Use VFS for file operations
}
```

## Memory Layout

### Exposed to libretro

| Memory ID | Description | Size | Purpose |
|-----------|-------------|------|---------|
| RETRO_MEMORY_SAVE_RAM | Backup RAM | Variable | Save data |
| RETRO_MEMORY_SYSTEM_RAM | Work RAM | 2MB | Cheats, inspection |
| RETRO_MEMORY_VIDEO_RAM | VRAM | 4MB | Graphics debugging |

### Memory Descriptors
```cpp
// Provide detailed memory map for achievement/cheat support
struct retro_memory_descriptor descriptors[] = {
    { RETRO_MEMDESC_SYSTEM_RAM, work_ram, 0, 0x00200000, 0, 0, 0x00200000, "Work RAM" },
    { RETRO_MEMDESC_VIDEO_RAM,  vram_a,   0, 0x00080000, 0, 0, 0x00080000, "VDP1 VRAM" },
    { RETRO_MEMDESC_VIDEO_RAM,  vram_b,   0, 0x00080000, 0, 0, 0x00080000, "VDP2 VRAM" },
    // ... more descriptors
};

struct retro_memory_map mmap = { descriptors, sizeof(descriptors) / sizeof(descriptors[0]) };
environ_cb(RETRO_ENVIRONMENT_SET_MEMORY_MAPS, &mmap);
```

## Build System Integration

### CMake Structure

```cmake
# Root CMakeLists.txt
cmake_minimum_required(VERSION 3.20)
project(Brimir VERSION 0.1.0 LANGUAGES CXX)

# C++20 required
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Options
option(BRIMIR_BUILD_TESTS "Build tests" OFF)
option(BRIMIR_STATIC_LINK "Static linking" OFF)

# Subdirectories
add_subdirectory(src/ymir)        # Ymir emulator
add_subdirectory(src/bridge)      # Bridge layer
add_subdirectory(src/libretro)    # Libretro interface

# Tests
if(BRIMIR_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Core library
add_library(brimir_libretro SHARED
    $<TARGET_OBJECTS:brimir_bridge>
    $<TARGET_OBJECTS:brimir_libretro_api>
)

target_link_libraries(brimir_libretro PRIVATE
    ymir_core
    # Other dependencies
)
```

### Makefile.libretro
```makefile
# For libretro buildbot compatibility
include Makefile.common

SOURCES_CXX := $(wildcard src/libretro/*.cpp) \
               $(wildcard src/bridge/*.cpp)

OBJECTS := $(SOURCES_CXX:.cpp=.o)

CXXFLAGS += -std=c++20 -DHAVE_LIBRETRO

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CXX) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(OBJECTS) $(TARGET)

.PHONY: all clean
```

## Error Handling

### Error Reporting Strategy
1. **Critical Errors**: Log and fail gracefully (return false from load/serialize)
2. **Warnings**: Log via libretro logging interface
3. **Debug Info**: Conditional compilation for verbose logging

```cpp
// Logging wrapper
void log_message(enum retro_log_level level, const char* fmt, ...) {
    if (!log_cb) return;
    
    va_list args;
    va_start(args, fmt);
    char buffer[4096];
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    
    log_cb(level, "[Brimir] %s\n", buffer);
}

// Usage
if (!m_ymirCore.LoadBIOS(bios_path)) {
    log_message(RETRO_LOG_ERROR, "Failed to load BIOS: %s", bios_path);
    return false;
}
```

## Performance Considerations

### Critical Path Optimization
- `retro_run()` must complete in < 16.67ms (for 60 FPS)
- Minimize allocations in hot paths
- Use efficient pixel format conversions
- Buffer audio appropriately

### Profiling Points
- Frame execution time
- Audio/video delivery overhead
- Input polling latency
- State serialization time

### Optimization Strategies
1. Profile-guided optimization (PGO)
2. SIMD for pixel format conversion
3. Lock-free audio buffer
4. Minimize memory copies

## Future Enhancements

### Hardware Acceleration
- OpenGL/Vulkan rendering path
- Texture upscaling
- Enhanced filtering

### Advanced Features
- Networked multiplayer
- RetroAchievements integration
- AI-assisted upscaling
- Debugger integration

---

## References

- [Libretro API Documentation](https://docs.libretro.com/)
- [Ymir Architecture](https://github.com/StrikerX3/Ymir)
- [Sega Saturn Hardware Docs](http://wiki.yabause.org/)

---

**Document Version:** 1.1  
**Last Updated:** 2025-11-27

