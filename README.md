# oled-sh1106-zephyr

### Dependencies

This module is dependent on OLED fonts, available at https://github.com/edgebr/oled-mono-fonts/tree/zephyr as a module
as well. The final `west.yml` will look like:

```yaml
      [...]
    - name: oled-mono-fonts
      remote: edgebr # or url to the mentioned repo, if remote is not set.
      revision: zephyr
      
    - name: oled-sh1106-zephyr
      remote: edgebr # or url to this repo, if remote is not set.
      revision: 3-wire-spi
      clone-depth: 1
      path: deps/display
```

On your project overlay/DTS, add the `display_spi` as a child node to your SPI (bit-banging for 3-wire mode) bus:

```DTS
# your SPI (bit-banging!) bus for 3-wire SH1106, e.g.:
spibb0: spibb0 {
  compatible = "zephyr,spi-bitbang";
	status = "okay";
  #address-cells = <1>;
  #size-cells = <0>;

  clk-gpios = <&gpio1 10 GPIO_ACTIVE_HIGH>;
  mosi-gpios = <&gpio1 1 GPIO_ACTIVE_HIGH>;
  cs-gpios = <&gpio1 7 GPIO_ACTIVE_LOW>;

  # This is the display node, used by this module.
	display_spi: display_spi@0 { 
		compatible = "vnd,spi-device";

		reg = <0>;
		spi-max-frequency = <1600000>;
		label = "OLED Display";
	};
};
```

> [!NOTE]
> You may try to plug the `display_spi` node in your SPI peripheral bus, but since the 3 wire SPI 
> configuration of SH1106 uses a 9-bit per packet protocol, it is usually unavailable for most SPI 
> peripherals.
