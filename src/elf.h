/* SPDX-License-Identifier: MIT
 * src/elf.h — Minimal ELF32 portability shim for non-Linux hosts (e.g. macOS).
 *
 * On Linux, elf_parser.c includes the system <elf.h> directly.
 * On all other platforms, this file provides the subset of ELF32 types and
 * constants required by elf_parser.c so that no glibc headers are needed.
 *
 * Guard: AOD_ELF_H  (avrOS-debug ELF header)
 */
#ifndef AOD_ELF_H
#define AOD_ELF_H

#include <stdint.h>

/* ── Scalar typedefs ─────────────────────────────────────────────────────── */
typedef uint16_t Elf32_Half;
typedef uint32_t Elf32_Word;
typedef uint32_t Elf32_Off;
typedef uint32_t Elf32_Addr;

/* ── ELF identification ──────────────────────────────────────────────────── */
#define ELFMAG      "\177ELF"
#define SELFMAG     4
#define EI_CLASS    4           /* index in e_ident: file class */
#define ELFCLASS32  1           /* 32-bit objects */

/* ── Object type ─────────────────────────────────────────────────────────── */
#define ET_EXEC     2           /* Executable file */

/* ── Machine type ────────────────────────────────────────────────────────── */
#define EM_AVR      0x0053      /* Atmel AVR 8-bit microcontroller */

/* ── Program-header type ─────────────────────────────────────────────────── */
#define PT_LOAD     1           /* Loadable segment */

/* ── Section-header type ─────────────────────────────────────────────────── */
#define SHT_SYMTAB  2           /* Symbol table */
#define SHT_STRTAB  3           /* String table */

/* ── Special section index ───────────────────────────────────────────────── */
#define SHN_UNDEF   0           /* Undefined / not present */

/* ── Symbol-table helpers ────────────────────────────────────────────────── */
#define ELF32_ST_BIND(i)  ((i) >> 4)
#define ELF32_ST_TYPE(i)  ((i) & 0xf)
#define STT_OBJECT  1
#define STT_FUNC    2
#define STB_GLOBAL  1

/* ── ELF file header (52 bytes) ──────────────────────────────────────────── */
typedef struct {
    unsigned char e_ident[16];  /* Magic + class + data + version + OS/ABI */
    Elf32_Half    e_type;       /* Object file type */
    Elf32_Half    e_machine;    /* Target architecture */
    Elf32_Word    e_version;    /* ELF version */
    Elf32_Addr    e_entry;      /* Entry-point virtual address */
    Elf32_Off     e_phoff;      /* Program-header table offset */
    Elf32_Off     e_shoff;      /* Section-header table offset */
    Elf32_Word    e_flags;      /* Processor-specific flags */
    Elf32_Half    e_ehsize;     /* Size of this header (52) */
    Elf32_Half    e_phentsize;  /* Size of one program-header entry */
    Elf32_Half    e_phnum;      /* Number of program-header entries */
    Elf32_Half    e_shentsize;  /* Size of one section-header entry */
    Elf32_Half    e_shnum;      /* Number of section-header entries */
    Elf32_Half    e_shstrndx;   /* Index of the section-name string table */
} Elf32_Ehdr;

/* ── Program header (32 bytes) ───────────────────────────────────────────── */
typedef struct {
    Elf32_Word    p_type;       /* Segment type */
    Elf32_Off     p_offset;     /* Offset in file */
    Elf32_Addr    p_vaddr;      /* Virtual address in memory */
    Elf32_Addr    p_paddr;      /* Physical address */
    Elf32_Word    p_filesz;     /* Size in file */
    Elf32_Word    p_memsz;      /* Size in memory */
    Elf32_Word    p_flags;      /* Segment flags */
    Elf32_Word    p_align;      /* Alignment */
} Elf32_Phdr;

/* ── Section header (40 bytes) ───────────────────────────────────────────── */
typedef struct {
    Elf32_Word    sh_name;      /* Section name (index into string table) */
    Elf32_Word    sh_type;      /* Section type */
    Elf32_Word    sh_flags;     /* Section flags */
    Elf32_Addr    sh_addr;      /* Section virtual address */
    Elf32_Off     sh_offset;    /* Offset in file */
    Elf32_Word    sh_size;      /* Section size in bytes */
    Elf32_Word    sh_link;      /* Link to another section */
    Elf32_Word    sh_info;      /* Additional info */
    Elf32_Word    sh_addralign; /* Alignment constraint */
    Elf32_Word    sh_entsize;   /* Entry size if section holds a table */
} Elf32_Shdr;

/* ── Symbol-table entry (16 bytes) ──────────────────────────────────────── */
typedef struct {
    Elf32_Word    st_name;      /* Symbol name (index into string table) */
    Elf32_Addr    st_value;     /* Symbol value / address */
    Elf32_Word    st_size;      /* Symbol size */
    unsigned char st_info;      /* Type and binding */
    unsigned char st_other;     /* Visibility */
    Elf32_Half    st_shndx;     /* Section index */
} Elf32_Sym;

#endif /* AOD_ELF_H */
