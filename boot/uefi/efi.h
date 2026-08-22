/* boot/uefi/efi.h — the slice of the UEFI spec this loader needs (DDR-886).
 *
 * Hand-declared rather than pulled from EDK2 or gnu-efi: these structures are
 * fixed by the UEFI specification, and 150 lines of declarations is a smaller,
 * more auditable surface than a submodule with its own build system. See
 * DDR-886 §2.
 *
 * Every field offset here is load-bearing — the firmware fills these tables, so
 * a wrong or missing member silently shifts every field after it.
 */
#pragma once
#include <stdint.h>

typedef uint64_t EFI_STATUS;
typedef void    *EFI_HANDLE;
typedef uint16_t CHAR16;

#define EFI_SUCCESS            0
#define EFI_BUFFER_TOO_SMALL   0x8000000000000005ull
#define EFI_INVALID_PARAMETER  0x8000000000000002ull

typedef struct { uint32_t d1; uint16_t d2, d3; uint8_t d4[8]; } EFI_GUID;

/* Memory types. Only Conventional and the two BootServices types are free for
 * the OS after ExitBootServices — see DDR-886 §3.4. */
#define EfiLoaderCode         1
#define EfiLoaderData         2
#define EfiBootServicesCode   3
#define EfiBootServicesData   4
#define EfiConventionalMemory 7

typedef struct {
    uint32_t type;
    uint32_t pad;
    uint64_t physical_start;
    uint64_t virtual_start;
    uint64_t number_of_pages;
    uint64_t attribute;
} EFI_MEMORY_DESCRIPTOR;

typedef struct {
    uint64_t signature;
    uint32_t revision, header_size, crc32, reserved;
} EFI_TABLE_HEADER;

struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
typedef EFI_STATUS (*EFI_TEXT_STRING)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, CHAR16 *);
typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    void           *reset;
    EFI_TEXT_STRING output_string;
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

typedef struct {
    EFI_TABLE_HEADER hdr;
    /* --- task priority (2) --- */
    void *raise_tpl, *restore_tpl;
    /* --- memory (5) --- */
    EFI_STATUS (*allocate_pages)(uint32_t type, uint32_t memtype, uint64_t pages, uint64_t *memory);
    void *free_pages;
    EFI_STATUS (*get_memory_map)(uint64_t *size, EFI_MEMORY_DESCRIPTOR *map,
                                 uint64_t *key, uint64_t *desc_size, uint32_t *desc_ver);
    EFI_STATUS (*allocate_pool)(uint32_t pooltype, uint64_t size, void **buf);
    EFI_STATUS (*free_pool)(void *buf);
    /* --- event & timer (6) --- */
    void *create_event, *set_timer, *wait_for_event, *signal_event, *close_event, *check_event;
    /* --- protocol handler (9) --- */
    void *install_protocol_interface, *reinstall_protocol_interface, *uninstall_protocol_interface;
    EFI_STATUS (*handle_protocol)(EFI_HANDLE handle, EFI_GUID *proto, void **iface);
    void *reserved2, *register_protocol_notify, *locate_handle, *locate_device_path,
         *install_configuration_table;
    /* --- image (5) --- */
    void *load_image, *start_image, *exit_image, *unload_image;
    EFI_STATUS (*exit_boot_services)(EFI_HANDLE image, uint64_t map_key);
} EFI_BOOT_SERVICES;

typedef struct {
    EFI_TABLE_HEADER hdr;
    CHAR16 *firmware_vendor;
    uint32_t firmware_revision, _pad;
    EFI_HANDLE console_in_handle;
    void *con_in;
    EFI_HANDLE console_out_handle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *con_out;
    EFI_HANDLE standard_error_handle;
    void *std_err;
    void *runtime_services;
    EFI_BOOT_SERVICES *boot_services;
    /* DDR-978: the two UEFI-spec fields that follow boot_services. This struct
     * previously stopped above, so the ACPI RSDP the firmware publishes here was
     * unreachable -- see DDR-978 §3.1. Read-only extension into a table the
     * firmware already provides; nothing before these offsets moves. */
    uint64_t num_table_entries;
    struct EFI_CONFIGURATION_TABLE *config_table;
} EFI_SYSTEM_TABLE;

typedef struct EFI_CONFIGURATION_TABLE {
    EFI_GUID vendor_guid;
    void    *vendor_table;
} EFI_CONFIGURATION_TABLE;

/* ACPI 2.0+ points at an XSDT-capable RSDP; ACPI 1.0 is the RSDT-only fallback. */
#define EFI_ACPI_20_TABLE_GUID \
    { 0x8868e871, 0xe4f1, 0x11d3, { 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 } }
#define EFI_ACPI_10_TABLE_GUID \
    { 0xeb9d2d30, 0x2d88, 0x11d3, { 0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d } }

/* Loaded image -> the device we were loaded from, so the kernel can be read off
 * the same volume rather than a hard-coded one. */
#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
    { 0x5B1B31A1, 0x9562, 0x11d2, { 0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } }
typedef struct {
    uint32_t   revision, _pad;
    EFI_HANDLE parent_handle;
    void      *system_table;
    EFI_HANDLE device_handle;
} EFI_LOADED_IMAGE_PROTOCOL;

#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID \
    { 0x0964e5b22, 0x6459, 0x11d2, { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } }

struct EFI_FILE_PROTOCOL;
typedef struct EFI_FILE_PROTOCOL {
    uint64_t revision;
    EFI_STATUS (*open)(struct EFI_FILE_PROTOCOL *self, struct EFI_FILE_PROTOCOL **out,
                       CHAR16 *name, uint64_t mode, uint64_t attrs);
    EFI_STATUS (*close)(struct EFI_FILE_PROTOCOL *self);
    void *deletef;
    EFI_STATUS (*read)(struct EFI_FILE_PROTOCOL *self, uint64_t *size, void *buf);
    void *write, *get_position, *set_position;
    EFI_STATUS (*get_info)(struct EFI_FILE_PROTOCOL *self, EFI_GUID *type,
                           uint64_t *size, void *buf);
} EFI_FILE_PROTOCOL;

typedef struct {
    uint64_t revision;
    EFI_STATUS (*open_volume)(void *self, EFI_FILE_PROTOCOL **root);
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

#define EFI_FILE_INFO_GUID \
    { 0x09576e92, 0x6d3f, 0x11d2, { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } }
typedef struct {
    uint64_t size;
    uint64_t file_size;
    uint64_t physical_size;
    uint8_t  create_time[16], last_access[16], modification[16];
    uint64_t attribute;
    CHAR16   file_name[1];
} EFI_FILE_INFO;

#define EFI_FILE_MODE_READ 0x0000000000000001ull

/* AllocatePages types */
#define AllocateAnyPages    0
#define AllocateAddress     2
