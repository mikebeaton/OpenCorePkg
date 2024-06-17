# OpenNetworkBoot

`OpenNetworkBoot` is an OpenCore boot entry protocol driver which provides
PXE and HTTP boot entries if the underlying firmware supports it, or if
the required network boot drivers have been loaded (e.g. by OpenCore). Using
additional network boot drivers (provided with OpenCore) when needed, HTTP
boot should be available on most firmware, even if not natively supported.

**Note**: In the above, and below, 'HTTP boot' refers to booting using either
`http://` or `https://`. The additional steps to configure a certificate for
`https://` (and to lock `OpenNetworkBoot` to `https://` only if required)
are covered below.

## PXE Boot

On almost all firmware, the drivers for PXE boot will already be present;
adding `OpenNetworkBoot.efi` to the OpenCore drivers should produce PXE
boot entries.

On some firmware (e.g. HP) the native network boot drivers are not loaded
if the system boots directly to OpenCore, therefore it is necessary to start
OpenCore via the firmware boot menu in order to see PXE boot entries.
(Alternatively, it should be possible to load the entire network boot stack as
provided with OpenCore, see OVMF instructions.)

## HTTP Boot

On most recent firmware, either no or only a few additional drivers are needed
for HTTP boot, as most required drivers are already present in firmware.

After adding `OpenNetworkBoot`, if no HTTP boot entries are seen, the next
thing to try is adding only the driver `HttpBootDxe`. If this does not work,
try adding all three of `HttpDxe`, `HttpUtilitiesDxe` and `HttpBootDxe`.

If this still does not work, proceed to the next section.

## Identifying missing drivers

The `dh` command in UEFI Shell (for instance `OpenShell` provided with
OpenCore) is useful for working out which drivers are missing for network
boot.

`dh -p LoadFile` shows available network boot entries. Handles with a device
path ending in an IPv4 or IPv6 address should indicate available PXE boot
options. Handles with a device path ending in `Uri(...)` should indicate
available HTTP boot options

 On some systems, there may be additional
`LoadFile` handles with non-standard paths. These may correspond, for
instance, to GUI network boot options. These cannot be used by
OpenNetworkBoot.

After identifying the handles for network boot entries,
the other handles just before and slightly after these, in the full
list of handles displayed by `dh`, should correspond to the currently loaded
network boot drivers. Examining the names printed by `dh` for these handles
and comparing them by eye to the available network boot drivers (see OVMF
section) can be used to identify any missing drivers.

Typical output when all drivers are loaded will look something like:

```
TODO
```

**Note 1**: On systems with reasonably fast console text output, the `-b`
option can be used with `dh` (and with most UEFI Shell commands) to
display results one page at a time.

**Note 2**: For systems with very slow console output, it may be more
convenient to pipe the results of `dh` to a file on a convenient file system
to examine later, within a booted OS. For example `dh > fs0:\foo` or:

```
> fs0:
> dh > foo
```

## OVMF

OVMF can be compiled with the following flags for full network boot support:

`-D NETWORK_HTTP_BOOT_ENABLE -D NETWORK_TLS_ENABLE -D NETWORK_IP6_ENABLE`

In addition `-D LINUX_LOADER` (for audk OVMF only) and
`-D DEBUG_ON_SERIAL_PORT` and a `DEBUG` or `NOOPT` build are
recommended.

If OVMF is compiled without network boot support, then network boot support
within OpenCore may be added by loading the full network boot stack provided
with OpenCore:

 - TODO: confirm, and list

### OVMF networking

To start OVMF with bridged network support the following macOS-specific
`qemu` options (which require `sudo`) may be used:

```
-netdev vmnet-bridged,id=mynet0,ifname=en0 \
-device e1000,netdev=mynet0,id=mynic0
```

**Note**: If any network boot clients (e.g. OVMF, VMWare) or server programs
(e.g. Apache, `dnsmasq`, WDS) are running on VMs, then it is normally easier
to set up and test these using bridged network support, which allows the VM to
appear as a separate device with its own IP address on the network.

PXE boot may also be tested in OVMF using the built-in TFTP/PXE server
available with the qemu user mode network stack, for example using the
following options:

```
-netdev user,id=net0,tftp=/Users/user/tftp,bootfile=/OpenShell.efi \
-device virtio-net-pci,netdev=net0
```

No equivalent options are available for HTTP boot, so to experiment with this,
a combination such as bridged networking and `dnsmasq` should be used.

### OVMF HTTPS certificate

When using `https://` as opposed to `http://`, a certificate must be
configured on the network boot client. Within the OVMF menus this may
be done using
`Device Manager/Tls Auth Configuration/Server CA Configuration/Enroll Cert/Enroll Cert Using File`.
(No GUID needs to be provided on this page - all zeroes will be
used - if only a single, test certificate is going to be configured.)

### Debugging

Building OVMF with the `-D DEBUG_ON_SERIAL_PORT` option and then passing the
`-serial stdio` option to qemu (and then scrolling back in the output as
needed, to the lines generated during a failed network boot) can be very
useful when trying to debug network boot setup.

## Configuring network boot

In standard PXE or HTTP boot, the normal network DHCP server responds to a
device's request for an IP address, but a separate DHCP helper service (often
running on a different machine from the DHCP server) responds to the device's
DHCP extension request to know which network boot program (NBP) to load. It is
possible (but actually less standard, even on enterprise networks; and
usually more complex) to configure the main DHCP server to respond to both
requests.

**Note 1**: The NBP for HTTP boot can be configured by specifying the `--uri`
flag for `OpenNetworkBoot`. When using this option, only an HTTP server (and
certificate, for HTTPS), needs to be set up; no DHCP helper service is
required.

**Note 2**: No equivalent option is provided for PXE boot, since the most
standard (and recommended) set up is for the program specifying the
NBP file and the program serving it via TFTP to be one and the same.

### PXE Boot

In PXE boot, then NBP is loaded via TFTP, which is a slow protocol, not suitable
for large files. Standard PXE boot NBPs tend to load further large files which
they need themselves with their own network stack, not via TFTP.

# WDS

Windows Deployment Services (WDS, which is incuded with Windows Server) can be
used to provide responses to PXE boot requests.

**Note**: WDS is now largely deprecated (only a few aspects are still
supported).

# dnsmasq

`dnsmasq` can be used to both provide the location of the NBP file and then
serve it by TFTP.

A basic `dnsmasq` PXE boot configuration is as follows:

```
TODO
```

A more advanced configuration might serve different files to different
machines, depending on their hardware id. (The same point applies to
HTTP boot.)

TODO: More info on which hardware id and where it is set and saved.

### HTTP Boot

Although `dnsmasq` arguably does not provide as full support for HTTP
boot as it does for PXE boot, its features can be used (or abused) to respond
to requests for the location of HTTP boot NBP files. An HTTP server
(such as Apache, or multiple other options) will be required to serve the
files.

Other options, such as TODO, may also be used.

A basic `dnsmasq` HTTP boot configuration is as follows:

```
TODO
```

### HTTPS Boot

Note that the certificate for validating https requests should be loaded into
firmware (***TODO***).

A normal https site would not serve files using a self-signed certificate
authority (CA), but since we are only attempting to serve files to HTTP boot
clients, in this case we can do so.

## Booting ISO and IMG files

Though not often noted in the documentation, the vast majority of HTTP Boot
implementations support loading `.iso` and `.img` files, which will be
automatically mounted as a ramdisk. If the mounted filesystem includes
`\EFI\BOOT\BOOTx64.efi` (or `\EFI\BOOT\BOOTIA32.efi` for 32-bit) then this
file will be loaded from the ramdisk and started.

The MIME types corresponding to `.iso` and `.img` files are:

 - `application/vnd.efi-iso`
 - `application/vnd.efi-img`

The MIME type for `.efi` files is:

 - `application/efi`

If the MIME type is not one of the above, then the corresponding file
extensions (`.iso`, `.img` and `.efi`) are used instead to identify the NBP
file type.

**Note**: Files which cannot be recognised by any of the above MIME types or
file extensions will not be loaded by standard HTTP boot drivers (including
the `HttpBootDxe` driver currently shipped with OpenCore).

## Booting DMG files

In order to allow booting macOS, `OpenNetworkBoot` includes additional support for
loading `.dmg` files via HTTP boot. If the NBP filename is `{filename}.dmg`
or `{filename}.chunklist` then the other file of this pair will be
automatically loaded, in order to allow DMG chunklist verification.

### Required MIME types

In order for `.dmg` and `.chunklist` files to be loaded by standard HTTP boot
drivers (including the `HttpBootDxe` driver currently shipped with OpenCore),
they must be served with the MIME type `application/efi`, otherwise they will
be treated as having an unknown file type and will not be loaded. (Note also
that `.dmg` files should not use the MIME types for ISO or IMG files, as above,
or else standard HTTP boot drivers will try to load, mount and boot from them
as the corresponding type of ramdisk, which will fail.)

For example, to set up the required MIME types when using Apache installed via
Homebrew on macOS, the following line should be added to
`/opt/homebrew/etc/httpd/mime.types`:

```
application/efi                                 efi dmg chunklist
```

### DmgLoading settings

The behaviour of `OpenNetworkBoot`'s DMG support depends on the OpenCore
`DmgLoading` setting as follows:

 - If `DmgLoading` is set to `Signed` then both `.chunklist` and `.dmg` files
must be available from the HTTP server. Either file can be specified as
the NBP, and the other matching file will be loaded afterwards, automatically.
 - If `DmgLoading` is set to `Disabled` and either of these two file extensions
are found as the NBP, then the HTTP boot process will be aborted. (Allowing
these files to load and then passing them to the OpenCore DMG loading process
would be pointless, as they would be rejected at that point anyway.)
 - If `DmgLoading` is set to `Any` and the NBP is `{filename}.dmg`, then no
attempt will be made to load the corresponding `{filename}.chunklist` as it
is not required. Specifying `{filename}.chunklist` as the NBP can be used to
force both files to be loaded and DMG verification to occur, in this situation.
The success or failure result of the verification can be seen in the OpenCore
log file, but since verification is not required (with this setting) the DMG
will load (assuming it is a non-corrupt DMG file) even when verification fails.

## NOTE: Security

Unfortunately, some or all of the recent CVEs affecting HTTP boot will almost
certainly apply to the EDK-2-derived HTTP boot drivers which are currently
shipped with OpenCore. Perhaps most importantly, these vulnerabilities render
HTTPS boot vulnerable to MITM attacks, by an attacker on your network. The
drivers shipped with OpenCore will be updated after fixes have appeared in
the EDK-2 code.
