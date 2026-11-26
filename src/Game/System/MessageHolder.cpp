#include "Game/System/MessageHolder.hpp"
#include "Game/NPC/TalkMessageInfo.hpp"
#include "Game/NPC/TalkNodeCtrl.hpp"
#include "Game/System/GameSystemObjHolder.hpp"
#include "Game/Util/FileUtil.hpp"
#include "Game/Util/SystemUtil.hpp"
#include <JSystem/JKernel/JKRArchive.hpp>
#ifdef __3DS__
#include "Game/System/Language.hpp"
#include <cstring>
#endif

#define SYSTEMMESSAGE_ARC "/Memory/SystemMessage.arc"
#define MESSAGE_ARC "/MessageData/Message.arc"
#ifdef __3DS__

// ##############################################################################################
// # The final Mailtoad thanks the player (owner's request, Nov 25 2026)                        #
// #                                                                                            #
// # On the Wii, after the Purple Coins the Mailtoad of PeachCastleFinalGalaxy asks whether     #
// # you want the staff's congratulations letter and sends it to the Wii Message Board, which   #
// # the 3DS does not have. Here his question (PeachCastleFinalGalaxy_Kinopio060) is replaced  #
// # by a thank-you in the Toads' cheering style, and the talk ends on it: no Yes/No, no        #
// # Message Board lines. The disc's text stays as it is on the SD; the change is made in       #
// # memory. KinopioAstro.cpp no longer prepares the letter.                                    #
// #                                                                                            #
// # The text is the converted (little-endian) message format of tools/3ds/builder/bmg.py:      #
// # a tag is 0x1A, then size | group << 8, the tag number, and its parameters as bytes.        #
// # Tags used: player name, form 1 ("Mr. Maaario"/"Mr. Luiiiigiii", group 5, tag 0) and the     #
// # page break (group 1, tag 1).                                                               #
// ##############################################################################################
#define PORT3DS_TAG_PLAYER_NAME_FORM1 L"\x001A\x0508\x0000\x0001"
#define PORT3DS_TAG_PAGE L"\x001A\x0106\x0001"

namespace {
    const char* cThanksMessageID = "PeachCastleFinalGalaxy_Kinopio060";

    const wchar_t cThanksEnglish[] = PORT3DS_TAG_PLAYER_NAME_FORM1 L"! Woo woo! You did it! \x266A" PORT3DS_TAG_PAGE
        L"\nThank you for playing!";
    const wchar_t cThanksFrench[] = PORT3DS_TAG_PLAYER_NAME_FORM1 L"! Bravo! Tu as r\x00E9" L"ussi! \x266A" PORT3DS_TAG_PAGE
        L"\nMerci d'avoir jou\x00E9" L"!";
    // Spanish: two lines on the first page, one is too wide for the balloon's text box (rendered
    // on the PC with the disc's TalkBalloonEvent and messagefont26, Nov 25 2026).
    const wchar_t cThanksSpanish[] = L"\x00A1" PORT3DS_TAG_PLAYER_NAME_FORM1 L"! \x00A1" L"Bien, bien!\n\x00A1" L"Lo lograste! \x266A"
        PORT3DS_TAG_PAGE L"\n\x00A1" L"Gracias por jugar!";

    const MessageData* sThanksData = nullptr;
    s32 sThanksIndex = -1;
    const wchar_t* sThanksText = nullptr;

    // Once, when the game's text is loaded: picks the text for the game's language and makes the
    // question the last message of its talk (it led to the Yes/No branch).
    void port3DSPrepareThanks(MessageData* pData) {
        sThanksIndex = pData->findMessageIndex(::cThanksMessageID);
        TalkNode* pNode = pData->findNode(::cThanksMessageID);

        if (sThanksIndex < 0 || pNode == nullptr) {
            sThanksIndex = -1;
            return;
        }

        const char* pLanguage = MR::getCurrentLanguagePrefix();
        if (strstr(pLanguage, "French") != nullptr) {
            sThanksText = ::cThanksFrench;
        } else if (strstr(pLanguage, "Spanish") != nullptr) {
            sThanksText = ::cThanksSpanish;
        } else {
            sThanksText = ::cThanksEnglish;
        }

        pNode->mNextIdx = 0xFFFF;
        sThanksData = pData;
    }
};  // namespace

// Called by MessageData::getMessage: the thank-you in place of the question.
static void port3DSReplaceText(const MessageData* pData, u16 index, TalkMessageInfo* pMessageInfo) {
    if (pData == ::sThanksData && index == ::sThanksIndex) {
        pMessageInfo->_0 = reinterpret_cast< u8* >(const_cast< wchar_t* >(::sThanksText));
    }
}
#endif

namespace {
    u8* getBlock(u32 magic, u8* pData) {
        u32 numBlocks = *reinterpret_cast< u32* >(pData + 0xc);
        pData += 0x20;

        for (u32 i = 0; i < numBlocks; i++) {
            u32 blockMagic = *reinterpret_cast< u32* >(pData);

            if (blockMagic == magic) {
                return pData;
            }

            u32 blockSize = *reinterpret_cast< u32* >(pData + 4);
            pData += blockSize;
        }

        return nullptr;
    }
};  // namespace

bool MessageData::getMessageDirect(TalkMessageInfo* pMessageInfo, const char* pMessage) const {
    s32 messageIndex = findMessageIndex(pMessage);

    if (messageIndex >= 0 && messageIndex < mInfoBlock->mItemCount) {
        getMessage(pMessageInfo, 0, messageIndex);

        return true;
    }

    return false;
}

bool MessageData::getMessage(TalkMessageInfo* pMessageInfo, u16, u16 infoToolIndex) const {
    u8* pInfoTool = getMessageInfoTool(infoToolIndex);
    pMessageInfo->_0 = reinterpret_cast< u8* >(mDataBlock + 1) + *reinterpret_cast< u32* >(pInfoTool);
    pMessageInfo->mCameraSetID = *reinterpret_cast< u16* >(pInfoTool + 4);
    pMessageInfo->_6 = *(pInfoTool + 6);
    pMessageInfo->mCameraType = *(pInfoTool + 7);
    pMessageInfo->mTalkType = *(pInfoTool + 8);
    pMessageInfo->_A = *(pInfoTool + 0xa);
    pMessageInfo->_B = *(pInfoTool + 0xb);
    pMessageInfo->mBalloonType = *(pInfoTool + 9);
#ifdef __3DS__
    port3DSReplaceText(this, infoToolIndex, pMessageInfo);
#endif

    return true;
}

TalkNode* MessageData::findNode(const char* pMessage) const {
    s32 messageIndex = findMessageIndex(pMessage);

    for (int i = 0; i < mFlowBlock->mNodeCount; i++) {
        TalkNode* pNode = reinterpret_cast< TalkNode* >(mFlowBlock + 1) + i;

        if (pNode->mNodeType == 1 && pNode->mIndex == messageIndex) {
            return pNode;
        }
    }

    return nullptr;
}

TalkNode* MessageData::getNode(u32 index) const {
    return reinterpret_cast< TalkNode* >(mFlowBlock + 1) + index;
}

TalkNode* MessageData::getBranchNode(u32 index) const {
    return reinterpret_cast< TalkNode* >(mFlowBlock + 1) + _14[index];
}

bool MessageData::isValidBranchNode(u32 index) const {
    return _14[index] != 0xffff;
}

u8* MessageData::getMessageInfoTool(int index) const {
    return reinterpret_cast< u8* >(mInfoBlock + 1) + mInfoBlock->mItemSize * index;
}

MessageHolder::MessageHolder() : mSystemMessageData(nullptr), mGameMessageData(nullptr), mSceneMessageData(nullptr) {
}

void MessageHolder::initSceneData() {
    mSceneMessageData = mGameMessageData;
}

void MessageHolder::destroySceneData() {
    mSceneMessageData = nullptr;
}

bool MessageSystem::getSystemMessageDirect(TalkMessageInfo* pMessageInfo, const char* pMessageId) {
    return MR::getGameSystemObjHolder()->mMessageHolder->mSystemMessageData->getMessageDirect(pMessageInfo, pMessageId);
}

bool MessageSystem::getGameMessageDirect(TalkMessageInfo* pMessageInfo, const char* pMessageId) {
    return MR::getGameSystemObjHolder()->mMessageHolder->mGameMessageData->getMessageDirect(pMessageInfo, pMessageId);
}

bool MessageSystem::getLayoutMessageDirect(TalkMessageInfo* pMessageInfo, const char* pMessageId) {
    return MR::getGameSystemObjHolder()->mMessageHolder->mGameMessageData->getMessageDirect(pMessageInfo, pMessageId);
}

MessageData* MessageSystem::getSceneMessageData() {
    return MR::getGameSystemObjHolder()->mMessageHolder->mSceneMessageData;
}

MessageData::MessageData(const char* pArchiveName)
    : mIDTable(nullptr), mInfoBlock(nullptr), mDataBlock(nullptr), _C(0), mFlowBlock(nullptr), _14(nullptr), _18(nullptr), mFLI1Block(nullptr) {
    JKRArchive* pArchive = nullptr;
    JKRHeap* pHeap = nullptr;
    MR::getMountedArchiveAndHeap(pArchiveName, &pArchive, &pHeap);

    u8* msgData = (u8*)pArchive->getResource("Message.bmg");
    u8* mapData = (u8*)pArchive->getResource(QUESTIONMARK_MAGIC, "MessageId.tbl");

    mIDTable = new JMapInfo();
    mIDTable->attach(mapData);

    mInfoBlock = (MessageInfoBlock*)::getBlock('INF1', msgData);
    mDataBlock = (MessageDataBlock*)::getBlock('DAT1', msgData);
    mFlowBlock = (MessageFlowBlock*)::getBlock('FLW1', msgData);

    if (mFlowBlock != nullptr) {
        _14 = reinterpret_cast< u16* >(reinterpret_cast< TalkNode* >(mFlowBlock + 1) + mFlowBlock->mNodeCount);
        _18 = reinterpret_cast< u8* >(_14 + mFlowBlock->_A);
    }

    mFLI1Block = (MessageFLI1Block*)::getBlock('FLI1', msgData);
}

inline JMapInfoIter end(const JMapInfo* pInfo) {
    return JMapInfoIter(pInfo, pInfo->getNumEntries());
}

s32 MessageData::findMessageIndex(const char* pMessage) const {
    JMapInfoIter iter = mIDTable->findElementBinary("MessageId", pMessage);

    // This *should* be mIDTable->end(), but I have had trouble
    // getting the compiler to inline that (see comment in JMapInfo.hpp)
    if (iter == end(mIDTable)) {
        return -1;
    }

    s32 messageIndex = -1;
    iter.getValue("Index", &messageIndex);

    return messageIndex;
}

void MessageHolder::initSystemData() {
    mSystemMessageData = new MessageData(SYSTEMMESSAGE_ARC);
}

void MessageHolder::initGameData() {
    MR::mountArchive(MESSAGE_ARC, nullptr);

    mGameMessageData = new MessageData(MESSAGE_ARC);
#ifdef __3DS__
    port3DSPrepareThanks(mGameMessageData);
#endif
}
