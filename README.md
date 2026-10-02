![](IMG/main.png)

A simple and inexpensive way to control Klipper via Wi-Fi, using a development module that already includes an ESP32 and a touchscreen.

## Components:

-  [ESP32 + Touch Display 1-2" DevBoard](https://s.click.aliexpress.com/e/_c38n5qfn) 
-  [Magnets cube 5x5 mm](https://s.click.aliexpress.com/e/_c37UIlXx) 
-  [Inserts M3 5x5.5 mm](https://s.click.aliexpress.com/e/_c3rIvPmt) Endstop X/Y
-  3D Printer


## Softwares
- [Microsoft Visula Studio Code](https://code.visualstudio.com/)

## Configuration

- [Install Klipper (Kiauh)](https://github.com/dw-0/kiauh)
- [CanBus](https://github.com/FaqT0tum/FlashForge_A5M_raspberry/blob/main/CanBus/canbus.md)


![](IMG/fan_part.png)


```bash
    [filament_switch_sensor runout_sensor]
    pause_on_runout: False
    switch_pin: !gpio3
    event_delay: 1.0
    runout_gcode:
        _FILAMENT_RUNOUT_EVENT
```


# Social

- [Instagram](https://www.instagram.com/sedimenti_/)
- [YouTube](https://www.youtube.com/channel/UCHJ_528ZI0BcSU-QA8kIJlg)
- [PrusaPrinter](https://www.printables.com/@SEDIMENTI_218145)
- [TikTok](https://www.tiktok.com/@sedimenti)

# Buy me a coffee

This project is Free so if you have the pleasure of supporting my next works I will be grateful fot the coffee.  
[PayPal](https://www.paypal.me/MattiaRusso308?locale.x=it_IT)
