#include "../include/jit_x64_backend.hpp"
#include <cstring>
#include <stdexcept>

#ifdef _WIN32
    #include <windows.h>
#else
    #include <sys/mman.h>
#endif

namespace brimir::jit {

X64CodeBuffer::X64CodeBuffer(size_t initial_capacity)
    : code_(nullptr)
    , size_(0)
    , capacity_(initial_capacity)
    , executable_(false)
{
    #ifdef _WIN32
        // Allocate with PAGE_READWRITE initially
        code_ = static_cast<uint8*>(VirtualAlloc(nullptr, capacity_,
                                                  MEM_COMMIT | MEM_RESERVE,
                                                  PAGE_READWRITE));
    #else
        code_ = static_cast<uint8*>(mmap(nullptr, capacity_,
                                         PROT_READ | PROT_WRITE,
                                         MAP_PRIVATE | MAP_ANONYMOUS,
                                         -1, 0));
    #endif
    
    if (!code_) {
        throw std::runtime_error("Failed to allocate code buffer");
    }
}

X64CodeBuffer::~X64CodeBuffer() {
    if (code_) {
        #ifdef _WIN32
            VirtualFree(code_, 0, MEM_RELEASE);
        #else
            munmap(code_, capacity_);
        #endif
    }
}

void X64CodeBuffer::Emit8(uint8 byte) {
    if (size_ >= capacity_) {
        Grow(capacity_ * 2);
    }
    code_[size_++] = byte;
}

void X64CodeBuffer::Emit16(uint16 value) {
    Emit8(value & 0xFF);
    Emit8((value >> 8) & 0xFF);
}

void X64CodeBuffer::Emit32(uint32 value) {
    Emit8(value & 0xFF);
    Emit8((value >> 8) & 0xFF);
    Emit8((value >> 16) & 0xFF);
    Emit8((value >> 24) & 0xFF);
}

void X64CodeBuffer::Emit64(uint64 value) {
    Emit32(value & 0xFFFFFFFF);
    Emit32((value >> 32) & 0xFFFFFFFF);
}

void X64CodeBuffer::Patch32(size_t position, uint32 value) {
    if (position + 4 > size_) {
        throw std::runtime_error("Patch position out of bounds");
    }
    code_[position] = value & 0xFF;
    code_[position + 1] = (value >> 8) & 0xFF;
    code_[position + 2] = (value >> 16) & 0xFF;
    code_[position + 3] = (value >> 24) & 0xFF;
}

void X64CodeBuffer::Reserve(size_t bytes) {
    if (size_ + bytes > capacity_) {
        Grow(size_ + bytes);
    }
}

void X64CodeBuffer::MakeExecutable() {
    if (executable_) return;
    
    #ifdef _WIN32
        DWORD old_protect;
        if (!VirtualProtect(code_, capacity_, PAGE_EXECUTE_READ, &old_protect)) {
            throw std::runtime_error("Failed to make code executable");
        }
    #else
        if (mprotect(code_, capacity_, PROT_READ | PROT_EXEC) != 0) {
            throw std::runtime_error("Failed to make code executable");
        }
    #endif
    
    // Flush instruction cache
    #ifdef _WIN32
        FlushInstructionCache(GetCurrentProcess(), code_, size_);
    #else
        __builtin___clear_cache(code_, code_ + size_);
    #endif
    
    executable_ = true;
}

void X64CodeBuffer::Grow(size_t min_capacity) {
    size_t new_capacity = capacity_;
    while (new_capacity < min_capacity) {
        new_capacity *= 2;
    }
    
    #ifdef _WIN32
        // Allocate new buffer
        uint8* new_code = static_cast<uint8*>(VirtualAlloc(nullptr, new_capacity,
                                                            MEM_COMMIT | MEM_RESERVE,
                                                            PAGE_READWRITE));
        if (!new_code) {
            throw std::runtime_error("Failed to grow code buffer");
        }
        
        // Copy existing code
        memcpy(new_code, code_, size_);
        
        // Free old buffer
        VirtualFree(code_, 0, MEM_RELEASE);
        
        code_ = new_code;
        capacity_ = new_capacity;
    #else
        // Use mremap on Linux (not available on all systems)
        throw std::runtime_error("Code buffer growth not implemented for this platform");
    #endif
}

} // namespace brimir::jit

