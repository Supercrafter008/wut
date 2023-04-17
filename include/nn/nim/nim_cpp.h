#pragma once
#include <wut.h>

/**
 * \defgroup nn_nim
 * \ingroup nn_nim
 * Network Installation Managment library (see nn::nim)
 * @{
 */

#ifdef __cplusplus

namespace nn {

namespace nim {

struct TitlePackageTaskConfig
{
    uint64_t titleId;
    uint32_t titleVersion;
    uint8_t titleType; // Use 1
    uint8_t downloadMedia; // 1 = MLC
    uint8_t hasTitleUpdate;
    uint8_t downloadMedia2; // 1 = MLC
    uint32_t oldTitleVersion;
    uint8_t ukn_0x14; // set to 0
    uint8_t ukn_0x15; // set to 1
    uint8_t postDownloadAction;
    uint8_t ukn_0x17;
};
WUT_CHECK_OFFSET(TitlePackageTaskConfig, 0x00, titleId);
WUT_CHECK_OFFSET(TitlePackageTaskConfig, 0x08, titleVersion);
WUT_CHECK_OFFSET(TitlePackageTaskConfig, 0x0c, titleType);
WUT_CHECK_OFFSET(TitlePackageTaskConfig, 0x0d, downloadMedia);
WUT_CHECK_OFFSET(TitlePackageTaskConfig, 0x0e, hasTitleUpdate);
WUT_CHECK_OFFSET(TitlePackageTaskConfig, 0x0f, downloadMedia2);
WUT_CHECK_OFFSET(TitlePackageTaskConfig, 0x10, oldTitleVersion);
WUT_CHECK_OFFSET(TitlePackageTaskConfig, 0x14, ukn_0x14);
WUT_CHECK_OFFSET(TitlePackageTaskConfig, 0x15, ukn_0x15);
WUT_CHECK_OFFSET(TitlePackageTaskConfig, 0x16, postDownloadAction);
WUT_CHECK_OFFSET(TitlePackageTaskConfig, 0x17, ukn_0x17);
WUT_CHECK_SIZE(TitlePackageTaskConfig, 0x18);

struct TitlePackageProgress
{
    uint64_t totalDownloadSize;
    uint64_t downloadedSize;
    uint64_t totalInstallSize;
    uint64_t installedSize;
    uint32_t totalNumEntries;
    uint32_t numInstalledEntries;
    uint32_t unk_0x28;
    uint32_t unk_0x2c;
    uint32_t state;
    uint32_t unk_0x34;
};
WUT_CHECK_OFFSET(TitlePackageProgress, 0x00, totalDownloadSize);
WUT_CHECK_OFFSET(TitlePackageProgress, 0x08, downloadedSize);
WUT_CHECK_OFFSET(TitlePackageProgress, 0x10, totalInstallSize);
WUT_CHECK_OFFSET(TitlePackageProgress, 0x18, installedSize);
WUT_CHECK_OFFSET(TitlePackageProgress, 0x20, totalNumEntries);
WUT_CHECK_OFFSET(TitlePackageProgress, 0x24, numInstalledEntries);
WUT_CHECK_OFFSET(TitlePackageProgress, 0x28, unk_0x28);
WUT_CHECK_OFFSET(TitlePackageProgress, 0x2c, unk_0x2c);
WUT_CHECK_OFFSET(TitlePackageProgress, 0x30, state);
WUT_CHECK_OFFSET(TitlePackageProgress, 0x34, unk_0x34);
WUT_CHECK_SIZE(TitlePackageProgress, 0x38);

class TitlePackageTask{
    public:
        TitlePackageTask(){
            packageId = 0xffffffffffffffff;
        }

        uint32_t
        Open(uint64_t packageId)
            asm("Open__Q3_2nn3nim16TitlePackageTaskFUL");
        
        void
        Close()
            asm("Close__Q3_2nn3nim16TitlePackageTaskFv");

        void
        GetProgress(TitlePackageProgress* progress)
            asm("GetProgress__Q3_2nn3nim16TitlePackageTaskCFPQ3_2nn3nim20TitlePackageProgress");

        uint32_t 
        StartForeground()
            asm("StartForeground__Q3_2nn3nim16TitlePackageTaskFv");

        uint32_t 
        StopForeground()
            asm("StopForeground__Q3_2nn3nim16TitlePackageTaskFv");

        uint32_t 
        StartInstall()
            asm("StartInstall__Q3_2nn3nim16TitlePackageTaskFv");

        uint64_t packageId;
};
WUT_CHECK_SIZE(TitlePackageTask, 0x08);

uint32_t 
Initialize()
    asm("Initialize__Q2_2nn3nimFv");

uint32_t
Finalize()
    asm("Finalize__Q2_2nn3nimFv");

uint32_t
GetNumTitlePackages()
    asm("GetNumTitlePackages__Q2_2nn3nimFv");

uint32_t
ListTitlePackages(uint64_t* packageId, uint32_t titleNum)
    asm("ListTitlePackages__Q2_2nn3nimFPULUi");

uint32_t
CalculateTitleInstallSize(int64_t* installSize, const TitlePackageTaskConfig* packageConfig, uint16_t const* unk, uint32_t unk1)
    asm("CalculateTitleInstallSize__Q2_2nn3nimFPLRCQ3_2nn3nim22TitlePackageTaskConfigPCUsUi");

uint32_t
RegisterTitlePackageTask(TitlePackageTaskConfig* config,  uint16_t const* unk, uint32_t unk1)
    asm("RegisterTitlePackageTask__Q2_2nn3nimFRCQ3_2nn3nim22TitlePackageTaskConfigPCUsUi");

uint32_t
UnregisterTitlePackageTask(uint64_t packageId)
    asm("UnregisterTitlePackageTask__Q2_2nn3nimFUL");

uint32_t
CancelAll()
    asm("CancelAll__Q2_2nn3nimFv");



} //namespace nim

} //namespace nn

#endif

/** @} */