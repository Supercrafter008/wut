#include "elf.h"
#include "utils.h"

#include <algorithm>
#include <fmt/format.h>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include <zlib.h>

constexpr auto DeflateMinSectionSize = 0x18u;
constexpr auto CodeBaseAddress = 0x02000000u;
constexpr auto DataBaseAddress = 0x10000000u;
constexpr auto LoadBaseAddress = 0xC0000000u;

struct ElfFile
{
   struct Section
   {
      elf::SectionHeader header;
      std::string name;
      std::vector<char> data;
   };

   elf::Header header;
   std::vector<std::unique_ptr<Section>> sections;
};

static int
getSectionIndex(ElfFile &file, const char *name)
{
   int index = 0;
   for (const auto &section : file.sections) {
      if (section->name == name) {
         return index;
      }

      ++index;
   }

   return -1;
}


static ElfFile::Section *
getSectionByType(ElfFile &file, elf::SectionType type)
{
   for (const auto &section : file.sections) {
      if (section->header.type == type) {
         return section.get();
      }
   }

   return nullptr;
}


static ElfFile::Section *
getSectionByName(ElfFile &file, const char *name)
{
   auto index = getSectionIndex(file, name);
   if (index == -1) {
      return nullptr;
   }

   return file.sections[index].get();
}


// https://stackoverflow.com/a/16569749
template<class TContainer>
bool begins_with(const TContainer& input, const TContainer& match)
{
   return input.size() >= match.size()
      && std::equal(match.begin(), match.end(), input.begin());
}


/**
 * Read the .elf file generated by compiler.
 */
static bool
readElf(ElfFile &file, const std::string &filename)
{
   std::ifstream in { filename, std::ifstream::binary };
   if (!in.is_open()) {
      fmt::print("Could not open {} for reading", filename);
      return false;
   }

   // Read header
   in.read(reinterpret_cast<char *>(&file.header), sizeof(elf::Header));

   if (file.header.magic != elf::HeaderMagic) {
      fmt::print("Invalid ELF magic header {:08X}", elf::HeaderMagic);
      return false;
   }

   if (file.header.fileClass != elf::ELFCLASS32) {
      fmt::print("Unexpected ELF file class {}, expected {}", file.header.fileClass, elf::ELFCLASS32);
      return false;
   }

   if (file.header.encoding != elf::ELFDATA2MSB) {
      fmt::print("Unexpected ELF encoding {}, expected {}", file.header.encoding, elf::ELFDATA2MSB);
      return false;
   }

   if (file.header.machine != elf::EM_PPC) {
      fmt::print("Unexpected ELF machine type {}, expected {}", file.header.machine, elf::EM_PPC);
      return false;
   }

   if (file.header.elfVersion != elf::EV_CURRENT) {
      fmt::print("Unexpected ELF version {}, expected {}", file.header.elfVersion, elf::EV_CURRENT);
      return false;
   }

   // Read section headers and data
   in.seekg(static_cast<size_t>(file.header.shoff));

   for (auto i = 0u; i < file.header.shnum; ++i) {
      file.sections.emplace_back(std::make_unique<ElfFile::Section>());
      auto &section = *file.sections.back();

      in.read(reinterpret_cast<char *>(&section.header), sizeof(elf::SectionHeader));

      if (!section.header.size || section.header.type == elf::SHT_NOBITS) {
         continue;
      }

      auto pos = in.tellg();
      in.seekg(static_cast<size_t>(section.header.offset));
      section.data.resize(section.header.size);
      in.read(section.data.data(), section.data.size());
      in.seekg(pos);
   }

   // Set section header names
   auto shStrTab = file.sections[file.header.shstrndx]->data.data();

   for (auto &section : file.sections) {
      section->name = shStrTab + section->header.name;
   }

   return true;
}


/**
 * Our linker script sometimes converts .bss from NOBITS to PROGBITS.
 */
static bool
fixBssNoBits(ElfFile &file)
{
   auto section = getSectionByName(file, ".bss");
   if (!section) {
      return true;
   }

   // Ensure there is actually all 0 in the .bss section
   for (const auto c : section->data) {
      if (c) {
         return false;
      }
   }

   // Set type back to NOBITS
   section->header.type = elf::SHT_NOBITS;
   section->header.offset = 0u;
   section->data.clear();
   return true;
}


/**
 * Reorder sections index.
 *
 * Expected order:
 *    NULL section
 *    > .syscall > .text
 *    > .fexports
 *    > .rodata > .data > .module_id > .bss
 *    > .rela.fexports > .rela.text > .rela.rodata > .rela.data
 *    > {.fimport, .dimport }
 *    > .symtab > .strtab > .shstrtab
 */
static bool
reorderSectionIndex(ElfFile &file)
{
   // Create a map of new index -> old index
   std::vector<std::size_t> sectionMap;
   sectionMap.push_back(0);

   // Code sections
   for (auto i = 0u; i < file.sections.size(); ++i) {
      if (file.sections[i]->header.type == elf::SHT_PROGBITS &&
          (file.sections[i]->header.flags & elf::SHF_EXECINSTR)) {
         sectionMap.push_back(i);
      }
   }

   // RPL exports
   for (auto i = 0u; i < file.sections.size(); ++i) {
      if (file.sections[i]->header.type == elf::SHT_RPL_EXPORTS) {
         sectionMap.push_back(i);
      }
   }

   // Read only data
   for (auto i = 0u; i < file.sections.size(); ++i) {
      if (file.sections[i]->header.type == elf::SHT_PROGBITS &&
          !(file.sections[i]->header.flags & elf::SHF_EXECINSTR) &&
          !(file.sections[i]->header.flags & elf::SHF_WRITE)) {
         sectionMap.push_back(i);
      }
   }

   // Writable data
   for (auto i = 0u; i < file.sections.size(); ++i) {
      if (file.sections[i]->header.type == elf::SHT_PROGBITS &&
          !(file.sections[i]->header.flags & elf::SHF_EXECINSTR) &&
          (file.sections[i]->header.flags & elf::SHF_WRITE)) {
         sectionMap.push_back(i);
      }
   }

   // BSS
   for (auto i = 0u; i < file.sections.size(); ++i) {
      if (file.sections[i]->header.type == elf::SHT_NOBITS) {
         sectionMap.push_back(i);
      }
   }

   // Relocations
   for (auto i = 0u; i < file.sections.size(); ++i) {
      if (file.sections[i]->header.type == elf::SHT_REL ||
          file.sections[i]->header.type == elf::SHT_RELA) {
         sectionMap.push_back(i);
      }
   }

   // RPL imports
   for (auto i = 0u; i < file.sections.size(); ++i) {
      if (file.sections[i]->header.type == elf::SHT_RPL_IMPORTS) {
         sectionMap.push_back(i);
      }
   }

   // Symtab and strtab
   for (auto i = 0u; i < file.sections.size(); ++i) {
      if (file.sections[i]->header.type == elf::SHT_SYMTAB ||
          file.sections[i]->header.type == elf::SHT_STRTAB) {
         sectionMap.push_back(i);
      }
   }

   if (sectionMap.size() != file.sections.size()) {
      fmt::print("Invalid section in elf file");
      return false;
   }

   // Apply the new ordering
   std::vector<std::unique_ptr<ElfFile::Section>> newSections;
   for (auto idx : sectionMap) {
      newSections.emplace_back(std::move(file.sections[idx]));
   }

   file.sections = std::move(newSections);

   // Now generate a reverse map, old index -> new index
   std::vector<uint16_t> mapOldToNew;
   mapOldToNew.resize(file.sections.size());
   for (auto i = 0u; i < sectionMap.size(); ++i) {
      mapOldToNew[sectionMap[i]] = static_cast<uint16_t>(i);
   }

   // Map file header.shstrndx
   file.header.shstrndx = mapOldToNew[file.header.shstrndx];

   // Map section header.link
   for (auto &section : file.sections) {
      section->header.link = mapOldToNew[section->header.link];
   }

   // Map relocation sections header.info
   for (auto &section : file.sections) {
      if (section->header.type != elf::SHT_RELA) {
         continue;
      }

      section->header.info = mapOldToNew[section->header.info];
   }

   // Map symbol.shndx
   for (auto &section : file.sections) {
      if (section->header.type != elf::SHT_SYMTAB) {
         continue;
      }

      auto symbols = reinterpret_cast<elf::Symbol *>(section->data.data());
      auto numSymbols = section->data.size() / sizeof(elf::Symbol);
      for (auto i = 0u; i < numSymbols; ++i) {
         auto shndx = symbols[i].shndx;
         if (shndx < elf::SHN_LORESERVE) {
            symbols[i].shndx = mapOldToNew[shndx];
         }
      }
   }

   return true;
}


/**
 * Generate SHT_RPL_FILEINFO section.
 */
static bool
generateFileInfoSection(ElfFile &file)
{
   elf::RplFileInfo info;
   info.version = 0xCAFE0402u;
   info.textSize = 0u;
   info.textAlign = 32u;
   info.dataSize = 0u;
   info.dataAlign = 4096u;
   info.loadSize = 0u;
   info.loadAlign = 4u;
   info.tempSize = 0u;
   info.trampAdjust = 0u;
   info.trampAddition = 0u;
   info.sdaBase = 0u;
   info.sda2Base = 0u;
   info.stackSize = 0x10000u;
   info.heapSize = 0x8000u;
   info.filename = 0u;
   info.flags = elf::RPL_IS_RPX; // TODO: Add .rpl support
   info.minVersion = 0x5078u;
   info.compressionLevel = -1;
   info.fileInfoPad = 0u;
   info.cafeSdkVersion = 0x51BAu;
   info.cafeSdkRevision = 0xCCD1u;
   info.tlsAlignShift = uint16_t { 0u };
   info.tlsModuleIndex = uint16_t { 0u };
   info.runtimeFileInfoSize = 0u;
   info.tagOffset = 0u;

   // Count file info textSize, dataSize, loadSize
   for (auto &section : file.sections) {
      auto size = static_cast<uint32_t>(section->data.size());

      if (section->header.type == elf::SHT_NOBITS) {
         size = section->header.size;
      }

      if (section->header.addr >= CodeBaseAddress &&
          section->header.addr < DataBaseAddress) {
         auto val = section->header.addr + section->header.size - CodeBaseAddress;
         if (val > info.textSize) {
            info.textSize = val;
         }
      } else if (section->header.addr >= DataBaseAddress &&
                 section->header.addr < LoadBaseAddress) {
         auto val = section->header.addr + section->header.size - DataBaseAddress;
         if (val > info.dataSize) {
            info.dataSize = val;
         }
      } else if (section->header.addr >= LoadBaseAddress) {
         auto val = section->header.addr + section->header.size - LoadBaseAddress;
         if (val > info.loadSize) {
            info.loadSize = val;
         }
      } else if (section->header.addr == 0 &&
                 section->header.type != elf::SHT_RPL_CRCS &&
                 section->header.type != elf::SHT_RPL_FILEINFO) {
         info.tempSize += (size + 128);
      }
   }

   info.textSize = align_up(info.textSize, info.textAlign);
   info.dataSize = align_up(info.dataSize, info.dataAlign);
   info.loadSize = align_up(info.loadSize, info.loadAlign);

   auto section = std::make_unique<ElfFile::Section>();
   section->header.name = 0u;
   section->header.type = elf::SHT_RPL_FILEINFO;
   section->header.flags = 0u;
   section->header.addr = 0u;
   section->header.offset = 0u;
   section->header.size = 0u;
   section->header.link = 0u;
   section->header.info = 0u;
   section->header.addralign = 4u;
   section->header.entsize = 0u;
   section->data.insert(section->data.end(),
                        reinterpret_cast<char *>(&info),
                        reinterpret_cast<char *>(&info + 1));
   file.sections.emplace_back(std::move(section));
   return true;
}


/**
 * Generate SHT_RPL_CRCS section.
 */
static bool
generateCrcSection(ElfFile &file)
{
   std::vector<be_val<uint32_t>> crcs;
   for (auto &section : file.sections) {
      auto crc = uint32_t { 0u };

      if (section->data.size()) {
         crc = crc32(0, Z_NULL, 0);
         crc = crc32(crc, reinterpret_cast<Bytef *>(section->data.data()), section->data.size());
      }

      crcs.push_back(crc);
   }

   // Insert a 0 crc for this section
   crcs.insert(crcs.end() - 1, 0);

   auto section = std::make_unique<ElfFile::Section>();
   section->header.name = 0u;
   section->header.type = elf::SHT_RPL_CRCS;
   section->header.flags = 0u;
   section->header.addr = 0u;
   section->header.offset = 0u;
   section->header.size = 0u;
   section->header.link = 0u;
   section->header.info = 0u;
   section->header.addralign = 4u;
   section->header.entsize = 4u;
   section->data.insert(section->data.end(),
                        reinterpret_cast<char *>(crcs.data()),
                        reinterpret_cast<char *>(crcs.data() + crcs.size()));
   // Insert before FILEINFO
   file.sections.insert(file.sections.end() - 1, std::move(section));
   return true;
}


static bool
getSymbol(ElfFile::Section &section, size_t index, elf::Symbol &symbol)
{
   auto symbols = reinterpret_cast<elf::Symbol *>(section.data.data());
   auto numSymbols = section.data.size() / sizeof(elf::Symbol);
   if (index >= numSymbols) {
      return false;
   }

   symbol = symbols[index];
   return true;
}


/**
 * Fix relocations.
 *
 * The Wii U does not support every type of relocation.
 */
static bool
fixRelocations(ElfFile &file)
{
   std::set<unsigned int> unsupportedTypes;
   auto result = true;

   for (auto &section : file.sections) {
      std::vector<elf::Rela> newRelocations;

      if (section->header.type != elf::SHT_RELA) {
         continue;
      }

      // Clear flags
      section->header.flags = 0u;

      auto &symbolSection = file.sections[section->header.link];
      auto &targetSection = file.sections[section->header.info];

      auto rels = reinterpret_cast<elf::Rela *>(section->data.data());
      auto numRels = section->data.size() / sizeof(elf::Rela);
      for (auto i = 0u; i < numRels; ++i) {
         auto info = rels[i].info;
         auto addend = rels[i].addend;
         auto offset = rels[i].offset;
         auto index = info >> 8;
         auto type = info & 0xFF;

         switch (type) {
         case elf::R_PPC_NONE:
         case elf::R_PPC_ADDR32:
         case elf::R_PPC_ADDR16_LO:
         case elf::R_PPC_ADDR16_HI:
         case elf::R_PPC_ADDR16_HA:
         case elf::R_PPC_REL24:
         case elf::R_PPC_REL14:
         case elf::R_PPC_DTPMOD32:
         case elf::R_PPC_DTPREL32:
         case elf::R_PPC_EMB_SDA21:
         case elf::R_PPC_EMB_RELSDA:
         case elf::R_PPC_DIAB_SDA21_LO:
         case elf::R_PPC_DIAB_SDA21_HI:
         case elf::R_PPC_DIAB_SDA21_HA:
         case elf::R_PPC_DIAB_RELSDA_LO:
         case elf::R_PPC_DIAB_RELSDA_HI:
         case elf::R_PPC_DIAB_RELSDA_HA:
            // All valid relocations on Wii U, do nothing
            break;

         /*
          * Convert a R_PPC_REL32 into two GHS_REL16
          */
         case elf::R_PPC_REL32:
         {
            elf::Symbol symbol;
            if (!getSymbol(*symbolSection, index, symbol)) {
               fmt::print("ERROR: Could not find symbol {} for fixing a R_PPC_REL32 relocation", index);
               result = false;
            } else {
               newRelocations.emplace_back();
               auto &newRel = newRelocations.back();

               // Modify current relocation to R_PPC_GHS_REL16_HI
               rels[i].info = (index << 8) | elf::R_PPC_GHS_REL16_HI;
               rels[i].addend = addend;
               rels[i].offset = offset;

               // Create a R_PPC_GHS_REL16_LO
               newRel.info = (index << 8) | elf::R_PPC_GHS_REL16_LO;
               newRel.addend = addend + 2;
               newRel.offset = offset + 2;
            }

            break;
         }

         default:
            // Only print error once per type
            if (!unsupportedTypes.count(type)) {
               fmt::print("ERROR: Unsupported relocation type {}", type);
               unsupportedTypes.insert(type);
            }
         }
      }

      section->data.insert(section->data.end(),
                           reinterpret_cast<char *>(newRelocations.data()),
                           reinterpret_cast<char *>(newRelocations.data() + newRelocations.size()));
   }

   return result && unsupportedTypes.size() == 0;
}


/**
 * Fix file header to look like an RPL file!
 */
static bool
fixFileHeader(ElfFile &file)
{
   file.header.magic = elf::HeaderMagic;
   file.header.fileClass = uint8_t { 1 };
   file.header.encoding = elf::ELFDATA2MSB;
   file.header.elfVersion = elf::EV_CURRENT;
   file.header.abi = elf::EABI_CAFE;
   memset(&file.header.pad, 0, 7);
   file.header.type = uint16_t { 0xFE01 };
   file.header.machine = elf::EM_PPC;
   file.header.version = 1u;
   file.header.flags = 0u;
   file.header.phoff = 0u;
   file.header.phentsize = uint16_t { 0 };
   file.header.phnum = uint16_t { 0 };
   file.header.shoff = align_up(static_cast<uint32_t>(sizeof(elf::Header)), 64);
   file.header.shnum = static_cast<uint16_t>(file.sections.size());
   file.header.shentsize = static_cast<uint16_t>(sizeof(elf::SectionHeader));
   file.header.ehsize = static_cast<uint16_t>(sizeof(elf::Header));
   file.header.shstrndx = static_cast<uint16_t>(getSectionIndex(file, ".shstrtab"));
   return true;
}


/**
 * Fix the .addralign field for sections.
 */
static bool
fixSectionAlign(ElfFile &file)
{
   for (auto &section : file.sections) {
      if (section->header.type == elf::SHT_PROGBITS) {
         section->header.addralign = 32u;
      } else if (section->header.type == elf::SHT_NOBITS) {
         section->header.addralign = 64u;
      } else if (section->header.type == elf::SHT_RPL_IMPORTS) {
         section->header.addralign = 4u;
      }
   }

   return true;
}

static bool
relocateSection(ElfFile &file,
                ElfFile::Section &section,
                uint32_t newSectionAddress)
{
   auto sectionSize = section.data.size() ? section.data.size() : static_cast<size_t>(section.header.size);
   auto oldSectionAddress = section.header.addr;
   auto oldSectionAddressEnd = section.header.addr + sectionSize;

   // Relocate symbols pointing into this section
   for (auto &symSection : file.sections) {
      if (symSection->header.type != elf::SectionType::SHT_SYMTAB) {
         continue;
      }

      auto symbols = reinterpret_cast<elf::Symbol *>(symSection->data.data());
      auto numSymbols = symSection->data.size() / sizeof(elf::Symbol);
      for (auto i = 0u; i < numSymbols; ++i) {
         auto type = symbols[i].info & 0xf;
         auto value = symbols[i].value;

         // Only relocate data, func, section symbols
         if (type != elf::STT_OBJECT &&
             type != elf::STT_FUNC &&
             type != elf::STT_SECTION) {
            continue;
         }

         if (value >= oldSectionAddress && value <= oldSectionAddressEnd) {
            symbols[i].value = (value - oldSectionAddress) + newSectionAddress;
         }
      }
   }

   // Relocate relocations pointing into this section
   for (auto &relaSection : file.sections) {
      if (relaSection->header.type != elf::SectionType::SHT_RELA) {
         continue;
      }

      auto rela = reinterpret_cast<elf::Rela *>(relaSection->data.data());
      auto numRelas = relaSection->data.size() / sizeof(elf::Rela);
      for (auto i = 0u; i < numRelas; ++i) {
         auto offset = rela[i].offset;

         if (offset >= oldSectionAddress && offset <= oldSectionAddressEnd) {
            rela[i].offset = (offset - oldSectionAddress) + newSectionAddress;
         }
      }
   }

   section.header.addr = newSectionAddress;
   return true;
}


/**
 * Fix the loader virtual addresses.
 *
 * Linker script won't put symtab & strtab sections in our loader address, so
 * we must fix that.
 *
 * Expected order:
 *    .fexports > .dexports > .symtab > .strtab > .shstrtab > {.fimport, .dimport}
 */
static bool
fixLoaderVirtualAddresses(ElfFile &file)
{
   auto addr = LoadBaseAddress;

   // All symbols pointing to this section require fixing

   if (auto section = getSectionByName(file, ".fexports")) {
      relocateSection(file, *section,
                      align_up(addr, section->header.addralign));
      addr += section->data.size();
   }

   if (auto section = getSectionByName(file, ".dexports")) {
      relocateSection(file, *section,
                      align_up(addr, section->header.addralign));
      addr += section->data.size();
   }

   if (auto section = getSectionByName(file, ".symtab")) {
      relocateSection(file, *section,
                      align_up(addr, section->header.addralign));
      section->header.flags |= elf::SHF_ALLOC;
      addr += section->data.size();
   }

   if (auto section = getSectionByName(file, ".strtab")) {
      relocateSection(file, *section,
                      align_up(addr, section->header.addralign));
      section->header.flags |= elf::SHF_ALLOC;
      addr += section->data.size();
   }

   if (auto section = getSectionByName(file, ".shstrtab")) {
      relocateSection(file, *section,
                      align_up(addr, section->header.addralign));
      section->header.flags |= elf::SHF_ALLOC;
      addr += section->data.size();
   }

   for (auto &section : file.sections) {
      if (section->header.type == elf::SHT_RPL_IMPORTS) {
         relocateSection(file, *section,
                         align_up(addr, section->header.addralign));
         addr += section->data.size();
      }
   }

   return true;
}

static bool
deflateSections(ElfFile &file)
{
   std::vector<char> chunk;
   chunk.resize(16 * 1024);

   for (auto &section : file.sections) {
      if (section->data.size() < DeflateMinSectionSize ||
          section->header.type == elf::SHT_RPL_CRCS ||
          section->header.type == elf::SHT_RPL_FILEINFO) {
         continue;
      }

      // Allocate space for the 4 bytes inflated size
      std::vector<char> deflated;
      deflated.resize(4);

      // Deflate section data
      auto stream = z_stream { };
      memset(&stream, 0, sizeof(stream));
      stream.zalloc = Z_NULL;
      stream.zfree = Z_NULL;
      stream.opaque = Z_NULL;
      deflateInit(&stream, 6);

      stream.avail_in = section->data.size();
      stream.next_in = reinterpret_cast<Bytef *>(section->data.data());

      do {
         stream.avail_out = static_cast<uInt>(chunk.size());
         stream.next_out = reinterpret_cast<Bytef *>(chunk.data());

         auto ret = deflate(&stream, Z_FINISH);
         if (ret == Z_STREAM_ERROR) {
            deflateEnd(&stream);
            return false;
         }

         deflated.insert(deflated.end(),
                         chunk.data(),
                         reinterpret_cast<char *>(stream.next_out));
      } while (stream.avail_out == 0);
      deflateEnd(&stream);

      // Set the inflated size at start of section
      *reinterpret_cast<be_val<uint32_t> *>(&deflated[0]) =
         static_cast<uint32_t>(section->data.size());

      // Update the section data
      section->data = std::move(deflated);
      section->header.flags |= elf::SHF_DEFLATED;
   }

   return true;
}

/**
 * Calculate section file offsets.
 *
 * Expected order:
 *    RPL_CRCS > RPL_FILEINFO >
 *    .rodata > .data > .module_id >
 *    .fexports > .dexports >
 *    .fimports > .dimports >
 *    .symtab > .strtab > .shstrtab >
 *    .syscall > .text >
 *    .rela.fexports > .rela.text > .rela.rodata > .rela.data
 */
static bool
calculateSectionOffsets(ElfFile &file)
{
   auto offset = file.header.shoff;
   offset += align_up(static_cast<uint32_t>(file.sections.size() * sizeof(elf::SectionHeader)), 64);

   if (auto section = getSectionByType(file, elf::SHT_RPL_CRCS)) {
      section->header.offset = offset;
      section->header.size = static_cast<uint32_t>(section->data.size());
      offset += section->header.size;
   }

   if (auto section = getSectionByType(file, elf::SHT_RPL_FILEINFO)) {
      section->header.offset = offset;
      section->header.size = static_cast<uint32_t>(section->data.size());
      offset += section->header.size;
   }

   // Data sections
   for (auto &section : file.sections) {
      if (section->header.type == elf::SHT_PROGBITS &&
          !(section->header.flags & elf::SHF_EXECINSTR)) {
         section->header.offset = offset;
         section->header.size = static_cast<uint32_t>(section->data.size());
         offset += section->header.size;
      }
   }

   // Exports
   for (auto &section : file.sections) {
      if (section->header.type == elf::SHT_RPL_EXPORTS) {
         section->header.offset = offset;
         section->header.size = static_cast<uint32_t>(section->data.size());
         offset += section->header.size;
      }
   }

   // Imports
   for (auto &section : file.sections) {
      if (section->header.type == elf::SHT_RPL_IMPORTS) {
         section->header.offset = offset;
         section->header.size = static_cast<uint32_t>(section->data.size());
         offset += section->header.size;
      }
   }

   // symtab & strtab
   for (auto &section : file.sections) {
      if (section->header.type == elf::SHT_SYMTAB ||
          section->header.type == elf::SHT_STRTAB) {
         section->header.offset = offset;
         section->header.size = static_cast<uint32_t>(section->data.size());
         offset += section->header.size;
      }
   }

   // Code sections
   for (auto &section : file.sections) {
      if (section->header.type == elf::SHT_PROGBITS &&
          (section->header.flags & elf::SHF_EXECINSTR)) {
         section->header.offset = offset;
         section->header.size = static_cast<uint32_t>(section->data.size());
         offset += section->header.size;
      }
   }

   // Relocation sections
   for (auto &section : file.sections) {
      if (section->header.type == elf::SHT_REL ||
          section->header.type == elf::SHT_RELA) {
         section->header.offset = offset;
         section->header.size = static_cast<uint32_t>(section->data.size());
         offset += section->header.size;
      }
   }

   return true;
}


/**
 * Write out the final RPL.
 */
static bool
writeRpl(ElfFile &file, const std::string &filename)
{
   auto shoff = file.header.shoff;

   // Write the file out
   std::ofstream out { filename, std::ofstream::binary };

   if (!out.is_open()) {
      fmt::print("Could not open {} for writing", filename);
      return false;
   }

   // Write file header
   out.seekp(0, std::ios::beg);
   out.write(reinterpret_cast<const char *>(&file.header), sizeof(elf::Header));

   // Write section headers
   out.seekp(shoff, std::ios::beg);
   for (const auto &section : file.sections) {
      out.write(reinterpret_cast<const char *>(&section->header), sizeof(elf::SectionHeader));
   }

   // Write sections
   for (const auto &section : file.sections) {
      if (section->data.size()) {
         out.seekp(section->header.offset, std::ios::beg);
         out.write(section->data.data(), section->data.size());
      }
   }

   return true;
}

int main(int argc, const char **argv)
{
   if (argc < 3) {
      fmt::print("Usage: {} <src elf> <dst rpl>", argv[0]);
      return -1;
   }

   auto src = std::string { argv[1] };
   auto dst = std::string { argv[2] };

   // Read elf into memory object!
   ElfFile elf;

   if (!readElf(elf, src)) {
      fmt::print("ERROR: readElf failed");
      return -1;
   }

   if (!fixBssNoBits(elf)) {
      fmt::print("ERROR: fixBssNoBits failed");
      return -1;
   }

   if (!reorderSectionIndex(elf)) {
      fmt::print("ERROR: reorderSectionIndex failed");
      return -1;
   }

   if (!fixRelocations(elf)) {
      fmt::print("ERROR: fixRelocations failed");
      return -1;
   }

   if (!fixSectionAlign(elf)) {
      fmt::print("ERROR: fixSectionAlign failed");
      return -1;
   }

   if (!fixLoaderVirtualAddresses(elf)) {
      fmt::print("ERROR: fixLoaderVirtualAddresses failed");
      return -1;
   }

   if (!generateFileInfoSection(elf)) {
      fmt::print("ERROR: generateFileInfoSection failed");
      return -1;
   }

   if (!generateCrcSection(elf)) {
      fmt::print("ERROR: generateCrcSection failed");
      return -1;
   }

   if (!fixFileHeader(elf)) {
      fmt::print("ERROR: fixFileHeader failed");
      return -1;
   }

   if (!deflateSections(elf)) {
      fmt::print("ERROR: deflateSections failed");
      return -1;
   }

   if (!calculateSectionOffsets(elf)) {
      fmt::print("ERROR: calculateSectionOffsets failed");
      return -1;
   }

   if (!writeRpl(elf, dst)) {
      fmt::print("ERROR: writeRpl failed");
      return -1;
   }

   return 0;
}
