#pragma once

#include <revolution/types.h>

class JASSeqReader {
public:
    JASSeqReader() {
        init();
    }
    void init();
    void init(void*);
    bool call(u32);
    bool loopStart(u32);
    bool loopEnd();
    bool ret();
    u32 readMidiValue();
    void* getStackPtr(u32 idx) const;

    void jump(u32 param_1) {
        mSeqCursor = mSeqBuff + param_1;
    }

    void jump(void* param_1) {
        mSeqCursor = (u8*)param_1;
    }

#ifdef __3DS__
    // Sequences stay big-endian on the 3DS: their operands are read byte by byte.
    static u32 readBE(const u8* p, int size) {
        u32 value = 0;
        for (int i = 0; i < size; i++) {
            value = value << 8 | p[i];
        }
        return value;
    }

    u32 get24(u32 param_0) const {
        return readBE(mSeqBuff + param_0, 3);
    }
#else
    u32 get24(u32 param_0) const {
        return (*(u32*)(mSeqBuff + param_0 - 1)) & 0xffffff;
    }
#endif

    u32* getBase() {
        return (u32*)mSeqBuff;
    }
    u32 getOffset() {
        return (u32)mSeqCursor - (u32)mSeqBuff;
    }
    u8* getAddr(u32 param_0) {
        return mSeqBuff + param_0;
    }
    u8 getByte(u32 param_0) const {
        return *(mSeqBuff + param_0);
    }
    // 3DS: sequence bytecode stays big-endian as on the disc, read byte by byte.
#ifdef __3DS__
    u16 get16(u32 param_0) const {
        return readBE(mSeqBuff + param_0, 2);
    }
    u32 get32(u32 param_0) const {
        return readBE(mSeqBuff + param_0, 4);
    }
#else
    u16 get16(u32 param_0) const {
        return *(u16*)(mSeqBuff + param_0);
    }
    u32 get32(u32 param_0) const {
        return *(u32*)(mSeqBuff + param_0);
    }
#endif
    u8* getCur() {
        return mSeqCursor;
    }
    u32 readByte() {
        return *mSeqCursor++;
    }
    u32 read16() {
#ifdef __MWERKS__
        return *((u16*)mSeqCursor)++;
#elif defined(__3DS__)
        u32 value = readBE(mSeqCursor, 2);
        mSeqCursor += 2;
        return value;
#else
        u16* value = (u16*)mSeqCursor;
        mSeqCursor += 2;
        return *value;
#endif
    }
    // 3DS: sequence bytecode stays big-endian as on the disc, read byte by byte.
#ifdef __3DS__
    u32 read24() {
        u32 value = readBE(mSeqCursor, 3);
        mSeqCursor += 3;
        return value;
    }
#else
    u32 read24() {
        mSeqCursor--;
#ifdef __MWERKS__
        return (*((u32*)mSeqCursor)++) & 0x00ffffff;
#else
        u32* value = (u32*)mSeqCursor;
        mSeqCursor += 4;
        return *value & 0x00ffffff;
#endif
    }
#endif
    u16 getLoopCount() const {
        if (mNumStacks == 0) {
            return 0;
        }
        return mLoopCounts[mNumStacks - 1];
    }

    /* 0x00 */ u8* mSeqBuff;
    /* 0x04 */ u8* mSeqCursor;
    /* 0x08 */ u32 mNumStacks;
    /* 0x0C */ u8* mStackPtrs[8];
    /* 0x2C */ u16 mLoopCounts[8];
};
