# WT02E40E ST-Link V2 OpenOCD Flash Helpers

These files are an experimental path for flashing the WT02E40E / nRF5340 over SWD with an ST-Link V2 and OpenOCD. 

The normal path is still the nRF5340 DK/J-Link path:

```powershell
west flash -d build
```

OpenOCD support for nRF5340/nRF53 is not as boring as nRF52 support. If your OpenOCD install does not contain `target/nordic/nrf53.cfg`, this will not work without a newer OpenOCD build or the relevant nRF53 support patches.

## Why there are two images

The nRF5340 has two Arm Cortex-M33 cores:

```text
CPUAPP  = application core
CPUNET  = network core used by BLE controller firmware
```

For this WT02E40E app:

```text
build/sta/zephyr/zephyr.hex              = application core app
build/hci_ipc/zephyr/zephyr.hex          = network core hci_ipc controller
build/hci_ipc/zephyr/merged_CPUNET.hex   = network core image, if generated
```

BLE advertising will not work unless the CPUNET image is flashed too.

## Wiring

Use a real 3.3 V supply for the WT02E40E. Do not rely on an ST-Link clone's 3.3 V pin for Wi-Fi current.

```text
ST-Link GND      -> WT02E40E GND
ST-Link SWDIO    -> WT02E40E SWDIO_NRF
ST-Link SWCLK    -> WT02E40E SWCLK_NRF
ST-Link NRST     -> WT02E40E RESET_NRF
ST-Link VTref    -> WT02E40E VDD_NRF / 3.3 V sense
```

Some ST-Link V2 clones expose the 3.3 V pin as output-only rather than true VTref. That is fine for some targets and cursed for others. If the board browns out, power the WT02E40E separately and only share GND/SWD/RESET.

## Files

```text
wt02e40e_nrf5340_stlink_dap.cfg
    First config to try. Uses interface/stlink-dap.cfg and target/nordic/nrf53.cfg.

wt02e40e_nrf5340_stlink_legacy_hla.cfg
    Fallback for older ST-Link HLA mode. May not work with nRF53.

flash_wt02e40e_stlink_openocd.bat
    Finds the sysbuild hex outputs and tries to flash CPUNET then CPUAPP.

flash_wt02e40e_stlink_openocd_legacy_hla.bat
    Same idea, but uses the legacy HLA config.

make_wt02e40e_merged_hex.bat
    Optional mergehex helper.
```

## Usage

From the project root after a sysbuild build:

```powershell
rmdir /s /q build
west build -b nrf7002dk/nrf5340/cpuapp . --sysbuild --pristine
```

Then run:

```powershell
openocd_stlink\flash_wt02e40e_stlink_openocd.bat build
```

Or with your current absolute path:

```powershell
openocd_stlink\flash_wt02e40e_stlink_openocd.bat C:\ncs\v3.3.1\nrf\samples\wifi\sta\build
```

## If it fails

If you see this:

```text
can't find target/nordic/nrf53.cfg
```

your OpenOCD does not have the nRF53 target support needed here.

If you see this:

```text
target nrf53.cpunet does not exist
```

your OpenOCD nRF53 config is different from the expected one. Run:

```powershell
openocd -f wt02e40e_nrf5340_stlink_dap.cfg -c "init" -c "targets" -c "shutdown"
```

Then edit the `.bat` target names to match the names OpenOCD prints.

If CPUNET programming fails but CPUAPP works, flash with J-Link/nrfjprog once to recover both cores, then use ST-Link only for app-core experiments. The nRF53 debug topology is just more annoying than nRF52.
