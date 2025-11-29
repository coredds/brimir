# Contributing to Brimir

Thank you for your interest in contributing to Brimir! This document provides guidelines and information for contributors.

## About Brimir

Brimir is a performance-focused fork of the Ymir emulator for the libretro ecosystem. While we maintain Ymir's cycle-accurate architecture and respect its GPL-3.0 license, Brimir actively develops performance optimizations and platform-specific enhancements. Our goal is to enable full-speed Saturn emulation on modern platforms and entry-level retro handhelds through techniques like JIT compilation and targeted optimizations.

Contributions that improve performance, compatibility, or platform reach while maintaining reasonable accuracy are especially welcome.

## Table of Contents

- [Code of Conduct](#code-of-conduct)
- [Getting Started](#getting-started)
- [Development Process](#development-process)
- [Coding Standards](#coding-standards)
- [Submitting Changes](#submitting-changes)
- [Testing](#testing)
- [Documentation](#documentation)
- [Community](#community)

## Code of Conduct

### Our Pledge

We are committed to providing a welcoming and inclusive environment for all contributors. We expect respectful and professional conduct from all community members.

### Expected Behavior

- Be respectful and considerate
- Welcome newcomers and help them get started
- Accept constructive criticism gracefully
- Focus on what is best for the project and community
- Show empathy towards other community members

### Unacceptable Behavior

- Harassment, discrimination, or offensive comments
- Personal attacks or trolling
- Publishing others' private information
- Any conduct that would be inappropriate in a professional setting

## Getting Started

### Prerequisites

Before contributing, ensure you have:

1. **Development Environment:**
   - C++20-capable compiler (GCC 11+, Clang 14+, MSVC 2022+)
   - CMake 3.20 or later
   - Git
   - vcpkg (will be set up as submodule)

2. **Knowledge:**
   - C++ programming (especially modern C++20)
   - Basic understanding of emulation concepts
   - Familiarity with git and GitHub workflow
   - (Optional) Experience with libretro API

3. **Accounts:**
   - GitHub account

### Setting Up Development Environment

1. **Fork the repository:**
   ```bash
   # Click "Fork" on GitHub, then clone your fork
   git clone https://github.com/coredds/brimir.git
   cd brimir
   ```

2. **Initialize submodules:**
   ```bash
   git submodule update --init --recursive
   ```

3. **Set up build environment:**
   ```bash
   # See README.md for detailed build instructions
   cmake -B build -S .
   cmake --build build
   ```

4. **Add upstream remote:**
   ```bash
   git remote add upstream https://github.com/coredds/brimir.git
   ```

## Development Process

### Finding Issues to Work On

- Look for issues labeled `good first issue` for beginner-friendly tasks
- Check `help wanted` label for areas needing assistance
- Review the [ROADMAP.md](docs/ROADMAP.md) for planned features
- Ask in Discussions if you're unsure where to start

### Creating a New Issue

Before creating an issue:
1. Search existing issues to avoid duplicates
2. Check if it's already covered in the PRD
3. Gather relevant information (system specs, error messages, steps to reproduce)

When creating an issue:
- Use a clear, descriptive title
- Provide detailed description
- Include steps to reproduce (for bugs)
- Add relevant labels
- Include system information if applicable

### Working on Features

1. **Create a branch:**
   ```bash
   git checkout -b feature/your-feature-name
   # or
   git checkout -b fix/your-bugfix-name
   ```

2. **Keep your branch updated:**
   ```bash
   git fetch upstream
   git rebase upstream/main
   ```

3. **Make focused commits:**
   - One logical change per commit
   - Write clear commit messages
   - Reference issues in commits (e.g., "Fix #123: ...")

4. **Test your changes:**
   - Build successfully on your platform
   - Run existing tests
   - Add new tests for new features
   - Test with actual games if applicable

## Coding Standards

### C++ Style Guide

#### General Principles
- Follow modern C++20 best practices
- Maintain consistency with existing Ymir codebase
- Prefer clarity over cleverness
- Write self-documenting code

#### Naming Conventions
```cpp
// Classes: PascalCase
class InputManager { };

// Functions/Methods: PascalCase
void UpdateFramebuffer();

// Variables: camelCase for local, m_ prefix for members
int frameCount;
int m_frameCount;  // member variable

// Constants: UPPER_SNAKE_CASE or kCamelCase
const int MAX_CONTROLLERS = 2;
constexpr int kBufferSize = 4096;

// Namespaces: lowercase
namespace brimir::bridge { }
```

#### Code Formatting
- Use `.clang-format` provided in the repository
- 4 spaces for indentation (no tabs)
- Opening braces on same line for functions/classes
- Maximum line length: 120 characters

```cpp
// Good
class Example {
public:
    void DoSomething() {
        if (condition) {
            // code
        }
    }
    
private:
    int m_value;
};
```

#### Comments
```cpp
// Use // for single-line comments
// Explain WHY, not WHAT (code should be self-explanatory)

/**
 * Use Doxygen-style comments for public APIs
 * @param input The input value
 * @return True if successful
 */
bool ProcessInput(int input);
```

#### Best Practices
- Use `nullptr` instead of `NULL` or `0`
- Prefer `enum class` over `enum`
- Use `const` and `constexpr` where appropriate
- Avoid raw pointers; use smart pointers
- Initialize all variables
- Use RAII for resource management
- Prefer standard library over custom implementations

```cpp
// Good
auto ptr = std::make_unique<Object>();
const auto value = CalculateValue();

// Avoid
Object* ptr = new Object();  // Manual memory management
int value;  // Uninitialized
```

### Libretro-Specific Guidelines

#### API Usage
- Always check return values from libretro callbacks
- Use appropriate log levels
- Handle null callbacks gracefully
- Follow libretro API conventions

```cpp
// Good
if (log_cb) {
    log_cb(RETRO_LOG_INFO, "[Brimir] Initializing core\n");
}

// Check environment callback success
bool success = environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var);
if (success && var.value) {
    // use var.value
}
```

#### Thread Safety
- Remember libretro cores are single-threaded from API perspective
- Use appropriate synchronization for internal threading
- Document any threading assumptions

### Git Commit Messages

Format:
```
<type>(<scope>): <subject>

<body>

<footer>
```

Types:
- `feat`: New feature
- `fix`: Bug fix
- `docs`: Documentation changes
- `style`: Code style changes (formatting, no logic change)
- `refactor`: Code refactoring
- `test`: Adding or updating tests
- `chore`: Maintenance tasks

Example:
```
feat(input): Add analog controller support

Implement mapping for Saturn 3D Control Pad analog inputs.
Maps analog stick to libretro analog input abstraction.

Closes #42
```

## Submitting Changes

### Pull Request Process

1. **Ensure your code:**
   - Builds without warnings
   - Passes all existing tests
   - Includes new tests for new features
   - Follows coding standards
   - Is well-documented

2. **Update documentation:**
   - Update relevant .md files
   - Add/update code comments
   - Update CHANGELOG.md

3. **Create pull request:**
   - Use a clear, descriptive title
   - Reference related issues
   - Describe what changed and why
   - Include testing performed
   - Add screenshots/videos if relevant

4. **PR Description Template:**
   ```markdown
   ## Description
   Brief description of changes
   
   ## Related Issues
   Fixes #123
   Related to #456
   
   ## Changes Made
   - Change 1
   - Change 2
   
   ## Testing Performed
   - Tested on Windows 11 x64
   - Tested with Games X, Y, Z
   - All unit tests pass
   
   ## Screenshots/Videos
   (if applicable)
   
   ## Checklist
   - [ ] Code builds without warnings
   - [ ] Tests added/updated
   - [ ] Documentation updated
   - [ ] Changelog updated
   ```

5. **Respond to feedback:**
   - Address review comments promptly
   - Discuss disagreements respectfully
   - Update PR based on feedback
   - Request re-review when ready

### Review Process

- Maintainers will review your PR
- Expect constructive feedback
- Multiple rounds of review may be needed
- CI must pass before merging
- At least one maintainer approval required

## Testing

### Running Tests

```bash
# Build with tests enabled
cmake -B build -S . -DBRIMIR_BUILD_TESTS=ON
cmake --build build

# Run all tests
cd build
ctest

# Run specific test
./tests/unit/test_core_wrapper
```

### Writing Tests

- Use the existing test framework
- Test public APIs
- Test edge cases and error conditions
- Aim for good coverage of new code

```cpp
// Example test
TEST_CASE("InputManager maps controller correctly") {
    InputManager manager;
    manager.SetControllerType(0, ControllerType::StandardPad);
    
    // Test button mapping
    manager.SetButtonState(0, RETRO_DEVICE_ID_JOYPAD_A, true);
    auto state = manager.GetControllerState(0);
    
    REQUIRE(state.buttons.a == true);
}
```

### Testing Games

When testing with actual games:
- Test with popular/demanding games
- Test save states at various points
- Test with different controller configurations
- Test multi-disc games
- Document any issues found

## Documentation

### Documentation Standards

- Keep documentation up-to-date with code changes
- Write clear, concise explanations
- Include code examples where helpful
- Use proper markdown formatting

### Types of Documentation

1. **Code Comments:**
   - Explain complex algorithms
   - Document assumptions
   - Note edge cases

2. **API Documentation:**
   - Use Doxygen-style comments
   - Document all public interfaces
   - Include usage examples

3. **User Documentation:**
   - README.md: Project overview
   - docs/guides/QUICKSTART.md: Setup and build instructions
   - User guides in docs/guides/

4. **Developer Documentation:**
   - docs/development/ARCHITECTURE.md: System design
   - docs/ROADMAP.md: Development roadmap
   - This file (CONTRIBUTING.md)

## Community

### Communication Channels

- **GitHub Issues:** Bug reports, feature requests
- **GitHub Discussions:** General questions, ideas
- **Email:** For sensitive issues only

### Getting Help

- Search existing documentation first
- Check closed issues for similar problems
- Ask in Discussions for general questions
- Be patient and respectful

### Recognizing Contributors

We value all contributions:
- Code contributions
- Bug reports and testing
- Documentation improvements
- Community support
- Translations (future)

Contributors will be:
- Listed in CONTRIBUTORS.md
- Credited in release notes
- Acknowledged in project documentation

## License

By contributing to Brimir, you agree that your contributions will be licensed under the GPL-3.0 license, consistent with the project's license.

### Contributor License Agreement

Your contributions must be:
- Your original work or properly attributed
- Compatible with GPL-3.0
- Free of any legal encumbrances

When submitting a PR, you certify that:
1. You have the right to submit the work
2. You understand it will be distributed under GPL-3.0
3. You grant the project maintainers a perpetual license to use your contribution

## Questions?

If you have questions not covered here:
- Open a Discussion on GitHub
- Contact maintainers directly

Thank you for contributing to Brimir!

---

**Note:** These guidelines may evolve as the project grows. Check back periodically for updates.

