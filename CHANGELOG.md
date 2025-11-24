# Changelog

All notable changes to the Brimir project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- Initial project structure
- Product Requirements Document (PRD)
- Architecture documentation
- Contributing guidelines
- Project README
- Core info file template

### Changed
- N/A

### Deprecated
- N/A

### Removed
- N/A

### Fixed
- N/A

### Security
- N/A

---

## Version History

### Version Numbering
- **Major**: Breaking changes, major feature additions
- **Minor**: New features, non-breaking changes
- **Patch**: Bug fixes, minor improvements

### Planned Milestones

#### [0.1.0] - Alpha Release - TBD
- First playable build
- Basic libretro API implementation
- Load and run simple games
- Software rendering
- Basic controller support

#### [0.2.0] - Beta Release - TBD
- Complete disc format support (CHD, BIN/CUE, ISO, etc.)
- Save state support
- All controller types
- Backup RAM/cartridge support
- Core options

#### [0.3.0] - Release Candidate - TBD
- Performance optimization
- Rewind support
- Complete feature set
- Comprehensive testing
- Documentation complete

#### [1.0.0] - Stable Release - TBD
- Production-ready core
- Official libretro repository inclusion
- 85%+ game compatibility
- Full platform support
- Stable API

---

## Development Notes

### Sync with Upstream Ymir
Brimir tracks the Ymir emulator project. Major Ymir updates will be incorporated regularly:

- Check for Ymir updates monthly
- Evaluate impact on Brimir integration
- Update submodule and adapt bridge layer as needed
- Document Ymir version in release notes

### Breaking Changes Policy
- Breaking changes will increment major version
- Deprecation warnings provided one minor version in advance
- Save state format changes handled with migration code
- Core option changes documented in release notes

---

## Template for Releases

```markdown
## [X.Y.Z] - YYYY-MM-DD

### Added
- New features

### Changed
- Changes to existing functionality

### Deprecated
- Features that will be removed in future

### Removed
- Removed features

### Fixed
- Bug fixes

### Security
- Security fixes

### Upstream
- Ymir version: vX.Y.Z
- Notable upstream changes
```

---

[Unreleased]: https://github.com/YOUR_USERNAME/brimir/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/YOUR_USERNAME/brimir/releases/tag/v1.0.0
[0.3.0]: https://github.com/YOUR_USERNAME/brimir/releases/tag/v0.3.0
[0.2.0]: https://github.com/YOUR_USERNAME/brimir/releases/tag/v0.2.0
[0.1.0]: https://github.com/YOUR_USERNAME/brimir/releases/tag/v0.1.0

