#include "Game/System/Language.hpp"
#include "Game/System/GameSystem.hpp"
#include "Game/System/GameSystemObjHolder.hpp"
#include "Game/Util/SingletonHolder.hpp"
#include "Game/Util/StringUtil.hpp"
#include "Game/Util/SystemUtil.hpp"
#include <revolution/sc.h>
#if defined(__3DS__)
#include "host.h"
#endif

namespace {
#if defined(__3DS__)
    s32 getSCLanguageTableIndex() {
#if (VERSION == RMGJ01 || VERSION == RMGE01)
        return rvlIsJapaneseRelease() ? 0 : 1;
#elif (VERSION == RMGP01)
        return 2;
#elif (VERSION == RMGK01)
        return 4;
#else
        return -1;
#endif
    }
#else
#if (VERSION == RMGJ01)
#define SCLanguage2GameLanguageTableIndex 0
#elif (VERSION == RMGE01)
#define SCLanguage2GameLanguageTableIndex 1
#elif (VERSION == RMGP01)
#define SCLanguage2GameLanguageTableIndex 2
#elif (VERSION == RMGK01)
#define SCLanguage2GameLanguageTableIndex 4
#else
#define SCLanguage2GameLanguageTableIndex -1
#endif
#endif
    const u8 cSCLanguage2GameLanguageTable[][10] = {
        {
            LANGUAGE_JPJAPANESE,
            LANGUAGE_USENGLISH,
            LANGUAGE_EUGERMAN,
            LANGUAGE_EUFRENCH,
            LANGUAGE_EUSPANISH,
            LANGUAGE_EUITALIAN,
            LANGUAGE_EUDUTCH,
#if (VERSION == RMGJ01 || VERSION == RMGE01 || VERSION == RMGP01)
            LANGUAGE_EUENGLISH,
            LANGUAGE_EUENGLISH,
            LANGUAGE_EUENGLISH,
#else
            LANGUAGE_CNSIMPCHINESE,
            LANGUAGE_CNSIMPCHINESE,
            LANGUAGE_KRKOREAN,
#endif
        },
        {
            LANGUAGE_JPJAPANESE,
            LANGUAGE_USENGLISH,
            LANGUAGE_EUGERMAN,
            LANGUAGE_USFRENCH,
            LANGUAGE_USSPANISH,
            LANGUAGE_EUITALIAN,
            LANGUAGE_EUDUTCH,
#if (VERSION == RMGJ01 || VERSION == RMGE01 || VERSION == RMGP01)
            LANGUAGE_USENGLISH,
            LANGUAGE_USENGLISH,
            LANGUAGE_USENGLISH,
#else
            LANGUAGE_CNSIMPCHINESE,
            LANGUAGE_CNSIMPCHINESE,
            LANGUAGE_KRKOREAN,
#endif
        },
        {
            LANGUAGE_JPJAPANESE,
            LANGUAGE_EUENGLISH,
            LANGUAGE_EUGERMAN,
            LANGUAGE_EUFRENCH,
            LANGUAGE_EUSPANISH,
            LANGUAGE_EUITALIAN,
            LANGUAGE_EUDUTCH,
#if (VERSION == RMGJ01 || VERSION == RMGE01 || VERSION == RMGP01)
            LANGUAGE_EUENGLISH,
            LANGUAGE_EUENGLISH,
            LANGUAGE_EUENGLISH,
#else
            LANGUAGE_CNSIMPCHINESE,
            LANGUAGE_CNSIMPCHINESE,
            LANGUAGE_KRKOREAN,
#endif
        },
#if (VERSION == RMGK01)
        {
            LANGUAGE_JPJAPANESE,
            LANGUAGE_USENGLISH,
            LANGUAGE_EUGERMAN,
            LANGUAGE_EUFRENCH,
            LANGUAGE_EUSPANISH,
            LANGUAGE_EUITALIAN,
            LANGUAGE_EUDUTCH,
            LANGUAGE_CNSIMPCHINESE,
            LANGUAGE_CNSIMPCHINESE,
            LANGUAGE_KRKOREAN,
        },
        {
            LANGUAGE_JPJAPANESE,
            LANGUAGE_USENGLISH,
            LANGUAGE_EUGERMAN,
            LANGUAGE_EUFRENCH,
            LANGUAGE_EUSPANISH,
            LANGUAGE_EUITALIAN,
            LANGUAGE_EUDUTCH,
            LANGUAGE_CNSIMPCHINESE,
            LANGUAGE_CNSIMPCHINESE,
            LANGUAGE_KRKOREAN,
        },
        {
            LANGUAGE_JPJAPANESE,
            LANGUAGE_USENGLISH,
            LANGUAGE_EUGERMAN,
            LANGUAGE_EUFRENCH,
            LANGUAGE_EUSPANISH,
            LANGUAGE_EUITALIAN,
            LANGUAGE_EUDUTCH,
            LANGUAGE_CNSIMPCHINESE,
            LANGUAGE_CNSIMPCHINESE,
            LANGUAGE_KRKOREAN,
        },
#endif
    };
    const Language cLanguages[] = {
        {LANGUAGE_JPJAPANESE, "JpJapanese"},       {LANGUAGE_USENGLISH, "UsEnglish"},
        {LANGUAGE_USSPANISH, "UsSpanish"},         {LANGUAGE_USFRENCH, "UsFrench"},
        {LANGUAGE_EUENGLISH, "EuEnglish"},         {LANGUAGE_EUSPANISH, "EuSpanish"},
        {LANGUAGE_EUFRENCH, "EuFrench"},           {LANGUAGE_EUGERMAN, "EuGerman"},
        {LANGUAGE_EUITALIAN, "EuItalian"},         {LANGUAGE_EUDUTCH, "EuDutch"},
#if (VERSION == RMGK01)
        {LANGUAGE_CNSIMPCHINESE, "CnSimpChinese"}, {LANGUAGE_KRKOREAN, "KrKorean"},
#endif
    };
};  // namespace

u32 MR::getDecidedLanguageFromIPL() {
    s32 language = SCGetLanguage();
    s32 i;

    if (language < 0) {
        i = 0;
    } else {
#if defined(__3DS__)
        s32 index = getSCLanguageTableIndex();
        s32 size = ARRAY_SIZE(::cSCLanguage2GameLanguageTable[index]);
#else
        s32 size = ARRAY_SIZE(::cSCLanguage2GameLanguageTable[SCLanguage2GameLanguageTableIndex]);
#endif

        if (language <= size) {
            i = language;
        } else {
            i = size;
        }
    }

#if defined(__3DS__)
    return ::cSCLanguage2GameLanguageTable[getSCLanguageTableIndex()][i];
#else
    return ::cSCLanguage2GameLanguageTable[SCLanguage2GameLanguageTableIndex][i];
#endif
}

u32 MR::getLanguage() {
    return SingletonHolder< GameSystem >::get()->mObjHolder->mLanguage;
}

u32 MR::getLanguageFromIPL() {
    return getLanguage() & LANGUAGE_MASK;
}

const char* MR::getCurrentLanguagePrefix() {
    u32 id = getLanguage();

    for (int i = 0; i < getLanguageNum(); i++) {
        if (id == ::cLanguages[i].mId) {
            return ::cLanguages[i].mName;
        }
    }

    return "English";
}

const char* MR::getCurrentRegionPrefix() {
    char prefix[3];
    MR::extractString(prefix, getCurrentLanguagePrefix(), sizeof(prefix) - 1, sizeof(prefix));

    if (MR::isEqualString(prefix, "Jp")) {
        return "Jp";
    }

    if (MR::isEqualString(prefix, "Us")) {
        return "Us";
    }

    if (MR::isEqualString(prefix, "Eu")) {
        return "Eu";
    }
#if (VERSION == RMGK01)
    if (MR::isEqualString(prefix, "Cn")) {
        return "Cn";
    }

    if (MR::isEqualString(prefix, "Kr")) {
        return "Kr";
    }
#endif
    return nullptr;
}

u32 MR::getLanguageNum() {
    return ARRAY_SIZE(::cLanguages);
}

const char* MR::getLanguagePrefixByIndex(u32 index) {
    return ::cLanguages[index].mName;
}
