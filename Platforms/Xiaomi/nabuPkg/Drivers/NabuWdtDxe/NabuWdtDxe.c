/** @file
  Nabu Mu-side boot log and APSS watchdog driver.

  Two jobs, both inert unless switched on:

    ESP file capture: at ReadyToBoot, sample PRAM_BASE, write it to
    \EFI\UNIXNU\BOOTLOG0.TXT / BOOTLOG1.TXT on the volume labelled NABUESP via
    BOOTLOG.TMP + rename, then stamp a fresh marker at PRAM_BASE. NABU_ESP_LOG 0
    compiles it out.

    Watchdog: arm the SM8150 APSS WDT at ExitBootServices when the resolved mode
    is 1 or 2. The mode comes from the UEFI variable NabuWdt (0 = no MMIO at all,
    1 = bite 240 s, 2 = test bite) and falls back to NABU_WDT_DEFAULT from the
    platform DSC when that variable is absent.

  The entry point never touches a block device: it runs before PartitionDxe and
  FatPkg are up, and an unbounded ReadBlocks/WriteBlocks there wedged the board
  with no reset source (2026-10-26). All storage access is in ReadyToBoot.
**/

#include <Uefi.h>
#include <Guid/EventGroup.h>
#include <Guid/FileInfo.h>
#include <Guid/FileSystemInfo.h>
#include <Protocol/SimpleFileSystem.h>
#include <Library/IoLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/DebugLib.h>
#include <Library/ArmLib.h>

#define NABU_APSS_WDT_BASE  0x17C10000ULL
#define WDT_EN              0x08
#define WDT_BARK_TIME       0x10
#define WDT_BITE_TIME       0x14
#define WDT_TICK_HZ         32768u
/* measured 2026-10-28: 240000 ms bit at boot+61 s => real clock is ~1/3.95 of the
 * assumed 32768 Hz tick; 540000 targets ~137 s */
#define NABU_WDT_BITE_MS    540000u
#define NABU_WDT_TEST_MS    15000u

/* Watchdog profile is a runtime choice, not a source edit:
 *   UEFI variable NabuWdt (UINT32)  0 = no MMIO at all, 1 = arm bite 240 s at
 *   ExitBootServices, 2 = test bite and spin. When the variable is absent or
 *   out of range we fall back to NABU_WDT_DEFAULT, which the platform DSC sets
 *   per module (see nabu.dsc [Components] BuildOptions). */
#define NABU_WDT_VAR_NAME  L"NabuWdt"

#ifndef NABU_WDT_DEFAULT
#define NABU_WDT_DEFAULT  0
#endif

#ifndef NABU_ESP_LOG
#define NABU_ESP_LOG  1
#endif

#define PRAM_BASE   0x9BC00000ULL
#define PRAM_MAGIC  0x4E425241u   /* NBRA */
#define PRAM_WORD1  0x5A5A5A5Au
#define PRAM_WORD2  0x20261026u
#define PRAM_WORD3  0xDEADBEEFu
#define PANIC_MAGIC 0x46554E4Bu   /* embedded panic header magic */

/* Kernel entry probes: three 16-byte records written from the entry patch at
 * 0x9BC7F000 + 16*idx (magic 'NBST', u32 stage, u64 CNTPCT). */
#define PRAM_PROBE_BASE   0x9BC7F000ULL
#define PRAM_PROBE_MAGIC  0x54534E42u
#define PRAM_PROBE_COUNT  3u

#define ESP_VOLUME_LABEL  L"NABUESP"
#define ESP_DIR_PATH      L"\\EFI\\UNIXNU"
#define ESP_SLOT0_NAME    L"BOOTLOG0.TXT"
#define ESP_SLOT1_NAME    L"BOOTLOG1.TXT"
#define ESP_WDT_CFG_NAME  L"WDT.CFG"
#define ESP_SLOT_LIMIT    (128u * 1024u)
#define ESP_HEADER_SIZE   128u
#define ESP_VERSION       1u

#define ESP_FLAG_RETAINED  0x01u
#define ESP_FLAG_PANIC     0x02u
#define ESP_FLAG_EMPTY     0x04u
#define ESP_FLAG_OTHER     0x08u

#pragma pack(1)
typedef struct {
  CHAR8   Magic[8];        /* "NABULOG1" */
  UINT32  Version;
  UINT32  HeaderSize;
  UINT32  Seq;             /* previous seq + 1, 0 when no slot could be read */
  UINT32  Crc32;           /* over the payload that follows the header */
  UINT64  Timestamp;       /* CNTPCT at write */
  CHAR8   TimeText[24];    /* RTC, "YYYY-MM-DD HH:MM:SS" */
  CHAR8   Source[12];      /* "mu-nabuwdt" */
  UINT32  Flags;
  UINT32  PayloadSize;
  UINT32  PrevWords[4];    /* raw read at PRAM_BASE */
  UINT8   Reserved[36];
} ESP_LOG_HEADER;
#pragma pack()

STATIC EFI_EVENT  mExitEvent;
STATIC UINT32     mWdtMode     = NABU_WDT_DEFAULT;
STATIC BOOLEAN    mWdtModeFromVar = FALSE;
STATIC CONST CHAR8  *mWdtModeSrc = "default";

/* Namespace for the NabuWdt variable. Deliberately local to this driver: nothing
 * else reads it, so it needs no entry in nabu.dec. */
STATIC CONST EFI_GUID  mNabuWdtVarGuid = {
  0x9a3f5c21, 0x4e77, 0x4b1a, { 0x8c, 0x25, 0x6f, 0x0d, 0x91, 0xe4, 0x77, 0x30 }
};

/* Read the profile before ExitBootServices, when runtime services still work here. */
STATIC
VOID
ResolveWdtMode (
  VOID
  )
{
  EFI_STATUS  Status;
  UINT32      Value;
  UINTN       Size;
  UINT32      Attributes;

  Value      = 0;
  Size       = sizeof (Value);
  Attributes = 0;
  Status     = gRT->GetVariable ((CHAR16 *)NABU_WDT_VAR_NAME, (EFI_GUID *)&mNabuWdtVarGuid,
                                 &Attributes, &Size, &Value);
  if (!EFI_ERROR (Status) && (Size == sizeof (Value)) && (Value <= 2u)) {
    mWdtMode        = Value;
    mWdtModeFromVar = TRUE;
    mWdtModeSrc     = "var";
  }
  DEBUG ((DEBUG_ERROR, "NabuWdt: mode=%u src=%a get=%r attrs=%x\n", mWdtMode,
          mWdtModeSrc, Status, Attributes));
}

STATIC
VOID
EFIAPI
OnExitBootServices (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  UINT32  biteMs;
  UINT32  ticks;

  (void)Event;
  (void)Context;
  if (mWdtMode == 0) {
    return;
  }
  biteMs = (mWdtMode == 2) ? NABU_WDT_TEST_MS : NABU_WDT_BITE_MS;
  ticks  = (biteMs * WDT_TICK_HZ) / 1000u;
  MmioWrite32 (NABU_APSS_WDT_BASE + WDT_BARK_TIME, ticks);
  MmioWrite32 (NABU_APSS_WDT_BASE + WDT_BITE_TIME, ticks);
  MmioWrite32 (NABU_APSS_WDT_BASE + WDT_EN, 1);
  /* Read-only snapshot of the timer block after arming: the kernel side needs a
   * measured baseline before anyone writes a ping register. */
  DEBUG ((DEBUG_ERROR, "NabuWdt: armed ticks=%u en=%x bark=%x bite=%x w0=%x w4=%x\n",
          ticks,
          MmioRead32 (NABU_APSS_WDT_BASE + WDT_EN),
          MmioRead32 (NABU_APSS_WDT_BASE + WDT_BARK_TIME),
          MmioRead32 (NABU_APSS_WDT_BASE + WDT_BITE_TIME),
          MmioRead32 (NABU_APSS_WDT_BASE),
          MmioRead32 (NABU_APSS_WDT_BASE + 0x04)));
  if (mWdtMode == 2) {
    for (;;) {
      __asm__ volatile ("yield" ::: "memory");
    }
  }
}

#if NABU_ESP_LOG
STATIC EFI_EVENT  mReadyEvent;
STATIC EFI_EVENT  mBeforeExitEvent;
STATIC BOOLEAN    mDone;

/* Volume label of one SimpleFileSystem handle; empty when it cannot be read. */
STATIC
VOID
EspVolumeLabel (
  IN  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs,
  OUT CHAR16                           *Label,
  IN  UINTN                             LabelChars
  )
{
  EFI_FILE_PROTOCOL     *Root = NULL;
  EFI_FILE_SYSTEM_INFO  *Info;
  UINTN                  Size;

  Label[0] = 0;
  if (EFI_ERROR (Fs->OpenVolume (Fs, &Root)) || Root == NULL) {
    return;
  }
  Size = SIZE_OF_EFI_FILE_SYSTEM_INFO + 256;
  Info = AllocateZeroPool (Size);
  if (Info != NULL) {
    if (!EFI_ERROR (Root->GetInfo (Root, &gEfiFileSystemInfoGuid, &Size, Info))) {
      StrnCpyS (Label, LabelChars, Info->VolumeLabel, LabelChars - 1);
    }
    FreePool (Info);
  }
  Root->Close (Root);
}

STATIC
EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *
FindNabuEsp (
  VOID
  )
{
  EFI_HANDLE                       *Handles = NULL;
  UINTN                             Count = 0;
  UINTN                             Index;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Found = NULL;
  CHAR8                             All[192];
  CHAR16                            Label[48];
  CHAR8                             Narrow[48];
  UINTN                             Used = 0;
  BOOLEAN                           Match;

  All[0] = 0;
  if (EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid,
                                          NULL, &Count, &Handles)) || Handles == NULL) {
    DEBUG ((DEBUG_ERROR, "NabuEsp: scan 0 handles\n"));
    return NULL;
  }
  for (Index = 0; Index < Count; ++Index) {
    Fs = NULL;
    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiSimpleFileSystemProtocolGuid,
                                        (VOID **)&Fs)) || Fs == NULL) {
      continue;
    }
    EspVolumeLabel (Fs, Label, ARRAY_SIZE (Label));
    Narrow[0] = 0;
    (void)UnicodeStrToAsciiStrS (Label, Narrow, sizeof (Narrow));
    if (Used + AsciiStrLen (Narrow) + 2 < sizeof (All)) {
      if (Used != 0) {
        All[Used++] = ',';
        All[Used]   = 0;
      }
      AsciiStrCatS (All, sizeof (All), Narrow);
      Used = AsciiStrLen (All);
    }
    Match = (StrCmp (Label, ESP_VOLUME_LABEL) == 0);
    if (Match && Found == NULL) {
      Found = Fs;
    }
  }
  gBS->FreePool (Handles);
  DEBUG ((DEBUG_ERROR, "NabuEsp: scan %u handles, label=%a%s\n", (UINT32)Count, All,
          Found != NULL ? "" : " (no match)"));
  return Found;
}

STATIC
UINT32
ReadSlotSeq (
  IN EFI_FILE_PROTOCOL  *Dir,
  IN CONST CHAR16       *Name
  )
{
  EFI_FILE_PROTOCOL  *File = NULL;
  ESP_LOG_HEADER      Header;
  UINTN               Size = sizeof (Header);
  UINT32              Seq = 0;

  if (EFI_ERROR (Dir->Open (Dir, &File, (CHAR16 *)Name, EFI_FILE_MODE_READ, 0)) || File == NULL) {
    return 0;
  }
  if (!EFI_ERROR (File->Read (File, &Size, &Header)) && Size == sizeof (Header) &&
      CompareMem (Header.Magic, "NABULOG1", 8) == 0 && Header.Version == ESP_VERSION) {
    Seq = Header.Seq;
  }
  File->Close (File);
  return Seq;
}

STATIC
UINT32
ClassifyProbe (
  IN  CONST UINT32  *Words,
  OUT CHAR8         *Text,
  IN  UINTN          TextSize
  )
{
  UINT32 r0 = Words[0], r1 = Words[1], r2 = Words[2], r3 = Words[3];

  if (r0 == PRAM_MAGIC) {
    AsciiSPrint (Text, TextSize, "retention=yes prev=%08x %08x %08x %08x", r0, r1, r2, r3);
    return ESP_FLAG_RETAINED;
  }
  if (r0 == PANIC_MAGIC) {
    UINT32 off = Words[4];
    UINT32 len = Words[5];
    UINTN  i, n;
    if (off >= 0x100000u || len > 0x100000u) {
      AsciiSPrint (Text, TextSize, "panic header invalid off=%u len=%u", off, len);
      return ESP_FLAG_PANIC;
    }
    n = AsciiSPrint (Text, TextSize, "panic off=%u len=%u text=\"", off, len);
    for (i = 0; i < 96 && i < len && (n + 2) < TextSize; ++i) {
      CHAR8 c = ((volatile CHAR8 *)(UINTN)(PRAM_BASE + off))[i];
      Text[n++] = (c >= 0x20 && c < 0x7f) ? c : '.';
    }
    Text[n++] = '"';
    Text[n]   = 0;
    return ESP_FLAG_PANIC;
  }
  if ((r0 | r1 | r2 | r3) == 0) {
    AsciiSPrint (Text, TextSize, "retention=no prev=00000000 00000000 00000000 00000000");
    return ESP_FLAG_EMPTY;
  }
  AsciiSPrint (Text, TextSize, "retention=? prev=%08x %08x %08x %08x", r0, r1, r2, r3);
  return ESP_FLAG_OTHER;
}

STATIC
EFI_STATUS
WriteEspLog (
  IN CONST UINT32  *Words,
  IN UINT32         Flags,
  IN CONST CHAR8   *Text
  )
{
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_PROTOCOL                *Root = NULL;
  EFI_FILE_PROTOCOL                *Dir = NULL;
  EFI_FILE_PROTOCOL                *File = NULL;
  EFI_FILE_PROTOCOL                *Old = NULL;
  ESP_LOG_HEADER                   *Header;
  EFI_STATUS                        Status;
  UINTN                             PayloadLen;
  UINTN                             Total;
  UINTN                             Written;
  UINT32                            Seq0, Seq1, Seq;
  CONST CHAR16                     *Name;
  CHAR8                             TimeText[24];
  EFI_TIME                          Time;

  Fs = FindNabuEsp ();
  if (Fs == NULL) {
    DEBUG ((DEBUG_ERROR, "NabuEsp: volume %s not found, nothing written\n", ESP_VOLUME_LABEL));
    return EFI_NOT_FOUND;
  }
  Status = Fs->OpenVolume (Fs, &Root);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = Root->Open (Root, &Dir, (CHAR16 *)ESP_DIR_PATH,
                       EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                       EFI_FILE_DIRECTORY);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "NabuEsp: open %s -> %r\n", ESP_DIR_PATH, Status));
    Root->Close (Root);
    return Status;
  }

  Seq0 = ReadSlotSeq (Dir, ESP_SLOT0_NAME);
  Seq1 = ReadSlotSeq (Dir, ESP_SLOT1_NAME);
  Seq  = ((Seq0 > Seq1) ? Seq0 : Seq1) + 1;
  Name = ((Seq & 1u) != 0) ? ESP_SLOT1_NAME : ESP_SLOT0_NAME;

  PayloadLen = AsciiStrLen (Text);
  if (PayloadLen > ESP_SLOT_LIMIT - ESP_HEADER_SIZE) {
    PayloadLen = ESP_SLOT_LIMIT - ESP_HEADER_SIZE;
  }
  Total  = ESP_HEADER_SIZE + PayloadLen;
  Header = AllocateZeroPool (Total);
  if (Header == NULL) {
    Dir->Close (Dir);
    Root->Close (Root);
    return EFI_OUT_OF_RESOURCES;
  }

  if (!EFI_ERROR (gRT->GetTime (&Time, NULL))) {
    AsciiSPrint (TimeText, sizeof (TimeText), "%04u-%02u-%02u %02u:%02u:%02u",
                 Time.Year, Time.Month, Time.Day, Time.Hour, Time.Minute, Time.Second);
  } else {
    AsciiSPrint (TimeText, sizeof (TimeText), "0000-00-00 00:00:00");
  }
  CopyMem (Header->Magic, "NABULOG1", 8);
  Header->Version      = ESP_VERSION;
  Header->HeaderSize   = ESP_HEADER_SIZE;
  Header->Seq          = Seq;
  Header->Timestamp    = ArmReadCntPct ();
  CopyMem (Header->TimeText, TimeText, 19);
  CopyMem (Header->Source, "mu-nabuwdt\0\0", 12);
  Header->Flags        = Flags;
  Header->PayloadSize  = (UINT32)PayloadLen;
  CopyMem (Header->PrevWords, Words, sizeof (Header->PrevWords));
  CopyMem ((UINT8 *)Header + ESP_HEADER_SIZE, Text, PayloadLen);
  Header->Crc32 = (PayloadLen != 0) ? CalculateCrc32 ((UINT8 *)Header + ESP_HEADER_SIZE, PayloadLen) : 0;

  /* Overwrite the chosen slot in place: no temp file and no rename. Deleting first keeps
   * the directory size exact when the new record is shorter than the old one. */
  if (!EFI_ERROR (Dir->Open (Dir, &Old, (CHAR16 *)Name,
                             EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0))) {
    Old->Delete (Old);
  }

  Status = Dir->Open (Dir, &File, (CHAR16 *)Name,
                      EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "NabuEsp: %s open -> %r\n", Name, Status));
  } else {
    UINT64 SizeSeen = 0;

    Written = Total;
    Status  = File->Write (File, &Written, Header);
    if (!EFI_ERROR (Status) && Written != Total) {
      Status = EFI_DEVICE_ERROR;
    }
    if (!EFI_ERROR (Status)) {
      /* Read the size back before closing: the directory entry is what the host sees. */
      EFI_FILE_INFO *Seen = AllocateZeroPool (SIZE_OF_EFI_FILE_INFO + 128);
      UINTN          SeenSize = SIZE_OF_EFI_FILE_INFO + 128;

      if (Seen != NULL) {
        if (!EFI_ERROR (File->GetInfo (File, &gEfiFileInfoGuid, &SeenSize, Seen))) {
          SizeSeen = Seen->FileSize;
        }
        FreePool (Seen);
      }
      if (SizeSeen != Total) {
        Status = EFI_DEVICE_ERROR;
      }
    }
    DEBUG ((DEBUG_ERROR, "NabuEsp: written=%u size=%u status=%r %s seq=%u\n",
            (UINT32)Written, (UINT32)SizeSeen, Status, Name, Seq));
    if (!EFI_ERROR (Status)) {
      Status = File->Flush (File);
    }
  }

  if (File != NULL) {
    File->Close (File);
  }
  FreePool (Header);
  Dir->Close (Dir);
  Root->Close (Root);
  return Status;
}

/* Append the kernel entry probes left by the previous boot to the capture text. */
STATIC
VOID
AppendProbes (
  IN OUT CHAR8  *Text,
  IN     UINTN   TextSize
  )
{
  volatile UINT32 *Rec = (volatile UINT32 *)(UINTN)PRAM_PROBE_BASE;
  UINTN            Len = AsciiStrLen (Text);
  UINTN            Found = 0;
  UINTN            Index;

  for (Index = 0; Index < PRAM_PROBE_COUNT; ++Index) {
    UINT32 Magic = Rec[Index * 4];
    UINT32 Stage = Rec[Index * 4 + 1];
    UINT64 Cnt   = ((UINT64)Rec[Index * 4 + 3] << 32) | Rec[Index * 4 + 2];

    if (Magic != PRAM_PROBE_MAGIC || Len + 48 >= TextSize) {
      continue;
    }
    Len += AsciiSPrint (Text + Len, TextSize - Len, "\nprobe%u=stage=%u cntpct=%lu",
                        (UINT32)Index, Stage, Cnt);
    Found++;
  }
  if (Found == 0 && Len + 16 < TextSize) {
    AsciiSPrint (Text + Len, TextSize - Len, "\nprobe0=none");
  }
}

/* This board has no UEFI Shell, so a variable alone would never change: the profile that
 * actually applies is read from \EFI\UNIXNU\WDT.CFG on the ESP (e.g. "mode=1"). The
 * variable keeps priority for the day a Shell exists. The volume only appears by
 * BeforeExitBootServices, which is where this runs. */
STATIC
VOID
ApplyWdtModeFile (
  VOID
  )
{
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_PROTOCOL                *Root = NULL;
  EFI_FILE_PROTOCOL                *Dir  = NULL;
  EFI_FILE_PROTOCOL                *File = NULL;
  CHAR8                             Text[64];
  UINTN                             Size;
  UINTN                             Index;

  if (mWdtModeFromVar) {
    return;
  }
  Fs = FindNabuEsp ();
  if ((Fs == NULL) || EFI_ERROR (Fs->OpenVolume (Fs, &Root)) || (Root == NULL)) {
    DEBUG ((DEBUG_ERROR, "NabuWdt: cfg volume unavailable\n"));
    return;
  }
  if (EFI_ERROR (Root->Open (Root, &Dir, (CHAR16 *)ESP_DIR_PATH, EFI_FILE_MODE_READ, 0))) {
    Root->Close (Root);
    return;
  }
  if (!EFI_ERROR (Dir->Open (Dir, &File, (CHAR16 *)ESP_WDT_CFG_NAME, EFI_FILE_MODE_READ, 0)) &&
      (File != NULL)) {
    Size = sizeof (Text) - 1;
    ZeroMem (Text, sizeof (Text));
    if (!EFI_ERROR (File->Read (File, &Size, Text)) && (Size > 0)) {
      for (Index = 0; Index + 6 < Size; ++Index) {
        UINT8  Digit;

        if (CompareMem (Text + Index, "mode=", 5) != 0) {
          continue;
        }
        Digit = (UINT8)(Text[Index + 5] - '0');
        if (Digit <= 2u) {
          mWdtMode    = Digit;
          mWdtModeSrc = "file";
          DEBUG ((DEBUG_ERROR, "NabuWdt: mode=%u read from ESP cfg\n", mWdtMode));
        } else {
          DEBUG ((DEBUG_ERROR, "NabuWdt: ESP cfg mode out of range\n"));
        }
        break;
      }
    }
    File->Close (File);
  } else {
    DEBUG ((DEBUG_ERROR, "NabuWdt: no ESP cfg file, mode stays %u (%a)\n",
            mWdtMode, mWdtModeSrc));
  }
  Dir->Close (Dir);
  Root->Close (Root);
}

/* Record the resolved watchdog profile in the ESP letter so a later round can prove
 * which mode a boot actually ran with, without trusting a source tree. */
STATIC
VOID
AppendWdtStamp (
  IN OUT CHAR8  *Text,
  IN     UINTN   TextSize
  )
{
  UINTN  Len;

  Len = AsciiStrLen (Text);
  if (Len + 64 >= TextSize) {
    return;
  }
  AsciiSPrint (Text + Len, TextSize - Len, "\nwdt=mode=%u bite=%u src=%a",
               mWdtMode,
               (mWdtMode == 2) ? NABU_WDT_TEST_MS : NABU_WDT_BITE_MS,
               mWdtModeSrc);
}

/* One capture attempt. ReadyToBoot runs before FatPkg installs the volume (the Mu log
 * shows the callback at 22.8 s and the FAT install at 23.4 s), so the same capture is
 * registered for BeforeExitBootServices as well. mDone keeps it to a single write and a
 * single pram stamp; a failed attempt leaves it clear so the later one can still land. */
STATIC
EFI_STATUS
NabuEspCapture (
  VOID
  )
{
  volatile UINT32 *Pram = (volatile UINT32 *)(UINTN)PRAM_BASE;
  UINT32           Words[6];
  CHAR8            Text[512];
  UINT32           Flags;
  EFI_STATUS       Status;

  if (mDone) {
    return EFI_SUCCESS;
  }
  Words[0] = Pram[0];
  Words[1] = Pram[1];
  Words[2] = Pram[2];
  Words[3] = Pram[3];
  if (Words[0] == PANIC_MAGIC) {
    Words[4] = Pram[4];
    Words[5] = Pram[5];
  } else {
    Words[4] = 0;
    Words[5] = 0;
  }

  Flags  = ClassifyProbe (Words, Text, sizeof (Text));
  AppendProbes (Text, sizeof (Text));
  AppendWdtStamp (Text, sizeof (Text));
  Status = WriteEspLog (Words, Flags, Text);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Pram[0] = PRAM_MAGIC;
  Pram[1] = PRAM_WORD1;
  Pram[2] = PRAM_WORD2;
  Pram[3] = PRAM_WORD3;
  mDone   = TRUE;
  DEBUG ((DEBUG_ERROR, "NabuEsp: stamped marker at %p\n", (VOID *)(UINTN)PRAM_BASE));
  return EFI_SUCCESS;
}

STATIC
VOID
EFIAPI
OnReadyToBoot (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  (void)Event;
  (void)Context;
  (void)NabuEspCapture ();
}

STATIC
VOID
EFIAPI
OnBeforeExitBootServices (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  (void)Event;
  (void)Context;
  /* Profile first, then the letter: the captured text has to carry the mode this boot
   * actually arms with. */
  ApplyWdtModeFile ();
  (void)NabuEspCapture ();
}
#endif

EFI_STATUS
EFIAPI
NabuWdtEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS status;

  (void)ImageHandle;
  (void)SystemTable;

  /* Variable first: everything below keys off mWdtMode, and GetVariable is a
   * runtime-services call, not block I/O, so the entry point stays I/O free. */
  ResolveWdtMode ();

#if NABU_ESP_LOG
  /* Registered first on purpose: the BeforeExitBootServices attempt is the one that
   * runs after FatPkg is up, so it gets the earliest slot this driver can give it. */
  status = gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, OnBeforeExitBootServices, NULL,
                               &gEfiEventBeforeExitBootServicesGuid, &mBeforeExitEvent);
  if (EFI_ERROR (status)) {
    DEBUG ((DEBUG_ERROR, "NabuEsp: before-exit event %r\n", status));
  }
#endif
  if (mWdtMode != 0) {
    MmioWrite32 (NABU_APSS_WDT_BASE + WDT_EN, 0);
  }

#if NABU_ESP_LOG
  status = gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, OnReadyToBoot, NULL,
                               &gEfiEventReadyToBootGuid, &mReadyEvent);
  if (EFI_ERROR (status)) {
    DEBUG ((DEBUG_ERROR, "NabuEsp: ready-to-boot event %r\n", status));
  }
#endif
  status = gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_NOTIFY, OnExitBootServices, NULL,
                               &gEfiEventExitBootServicesGuid, &mExitEvent);
  return status;
}
