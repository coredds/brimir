#pragma once

/**
@file
@brief Mode 2 track subheader structure definition.

The subheader contains four fields, one byte each: file number, channel number, submode and coding information.

The file number field identifies sectors that belong to a file. Each number corresponds to a file. The file 0x00 is a
special case used for low-level data structures like CD-ROM path table records, directory records, volume descriptors,
and contiguous files.

The channel number field identifies data streams belonging to a file. This can be used to interleave a video tracks and
multiple audio tracks for different languages, for example.

The submode field is a bitmask indicating features of the sector:
  bit  code  name
   0   EOR   End Of Record
   1   V     Video
   2   A     Audio
   3   D     Data
   4   T     Trigger
   5   F     Form
   6   RT    Real Time Sector
   7   EOF   End Of File

- EOR indicates the last logical sector of a record.
- V, A and D identify the type of content of this sector. Only one of these must be set, except on empty sectors where
  all three must be zero.
- T is used to for synchronization when using multiple coding information types.
- F indicates if the sector is in Form 1 (clear) or Form 2 (set). Data tracks are always Form 1 and Audio tracks are
  always Form 2.
- RT instructs the reader to not interrupt real-time processing of CD-ROM XA data.
- EOF indicates the last sector of a file.

The coding information field is typically used to describe the format of the data contained in the sector. Sega Saturn
software is free to use this at will.
*/

#include <brimir/core/types.hpp>

namespace brimir::media {

/// @brief Mode 2 track subheader
struct Subheader {
    uint8 fileNum;    ///< File number
    uint8 chanNum;    ///< Channel number
    uint8 submode;    ///< Submode
    uint8 codingInfo; ///< Coding information
};

} // namespace brimir::media
