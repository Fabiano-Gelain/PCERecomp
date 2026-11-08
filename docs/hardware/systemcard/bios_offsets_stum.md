<!-- Source: https://stum.de/2025/pcenginebiosoffsets/ — Michael Stum, 2025. License CC BY-NC-SA 4.0. Converted from HTML. -->

The PC Engine CD-ROM² consists of a HuCard containing a BIOS, which interfaces with the actual CD-ROM drive and serves as a bare-bones operating system of sorts for CD-ROM games.
Like all HuCards, the System Card is mapped to the $E000-$FFFF memory range on startup (MPR#7 = $00), and developers should not change this when calling BIOS functions since the code assume that base address. The BIOS also assumes/requires that MPR#0 is $FF and MPR#1 is $F8 to map the I/O and Work RAM to the beginning of address space, which is standard anyway.
To keep the programming interface compatible between revisions of the System Card, it utilizes a jump table. For example, if a developer wants to call the cd_seek, they are supposed to always JSR to address $E00C, which consists of a JMP $xxyy call to jump to the actual body of the method. That way, if the actual location of the cd_seek method changes (because previous methods were refactored), game code still works.
In order to help with debugging (e.g., trying to figure out why Altered Beast only works with the 1.0 card), I've made a list of each BIOS method and it's actual location on the various system cards for the PC Engine and Turbografx-16.
I have the manual for the System Card 1.0, with the official names. The System Card 2.0 added 4 additional methods. I don't know if they have official names, I named them this:
- ad_stream_start and ad_stream_poll allow for asyncronous streaming from the CD to ADPCM
- ma_mul16s is a signed version of ma_mul16u (signed multiplication of two 16-bit values to produce a 32-bit result)
- ma_div8u divides two 8-bit values to produce an 8-bit result
The Arcade Card is "just" a 3.0 card with more RAM on it, which is mapped through additional I/O registers - but the actual CD BIOS methods are unchanged from the regular 3.0 Super CD-ROM² card.
Aside: Something similar was done on the Amiga, where the only address a developer should ever hardcode is ExecBase. It's an elegant way to version the code, although there's a penalty on each and every BIOS call due to the extra layer of indirection. Thankfully the PC Engine's CPU was clocked very fast (for 6502 standards) and CD method calls were relatively infrequent.
## Method Addresses across CD-ROM² System Cards
| Method | JSR | 1.0 | 2.0 | 2.1 | 3.0 | TG2.0 | TG3.0 |
| cd_boot | $E000 | $E0E7 | $E0F3 | $E0F3 | $E0F3 | $E0F3 | $E0F3 |
| cd_reset | $E003 | $E95C | $E8E1 | $E8E1 | $E8E3 | $E8FA | $E8FC |
| cd_base | $E006 | $EC1C | $EB8F | $EB8F | $EB8F | $EBA8 | $EBA8 |
| cd_read | $E009 | $EC6D | $EC05 | $EC05 | $EC05 | $EC1E | $EC1E |
| cd_seek | $E00C | $EE49 | $EDCB | $EDCB | $EDCB | $EDE4 | $EDE4 |
| cd_exec | $E00F | $EC57 | $EBEC | $EBEC | $EBEC | $EC05 | $EC05 |
| cd_play | $E012 | $EE96 | $EE10 | $EE10 | $EE10 | $EE29 | $EE29 |
| cd_search | $E015 | $EFB1 | $EF34 | $EF34 | $EF34 | $EF4D | $EF4D |
| cd_pause | $E018 | $F01B | $EF94 | $EF94 | $EF94 | $EFAD | $EFAD |
| cd_stat | $E01B | $F3A7 | $F33D | $F347 | $F347 | $F360 | $F360 |
| cd_subq | $E01E | $F047 | $EFBF | $EFBF | $EFBF | $EFD8 | $EFD8 |
| cd_dinfo | $E021 | $F081 | $EFF1 | $EFF1 | $EFF1 | $F00A | $F00A |
| cd_contnts | $E024 | $F13A | $F0A9 | $F0A9 | $F0A9 | $F0C2 | $F0C2 |
| cd_subrd | $E027 | $F3B5 | $F34A | $F354 | $F354 | $F36D | $F36D |
| cd_pcmrd | $E02A | $F3C5 | $F35A | $F364 | $F364 | $F37D | $F37D |
| cd_fade | $E02D | $F3DA | $F36F | $F379 | $F379 | $F392 | $F392 |
| ad_reset | $E030 | $F3E0 | $F375 | $F37F | $F37F | $F398 | $F398 |
| ad_trans | $E033 | $F3F4 | $F389 | $F393 | $F393 | $F3AC | $F3AC |
| ad_read | $E036 | $F467 | $F3FD | $F407 | $F407 | $F420 | $F420 |
| ad_write | $E039 | $F566 | $F4CE | $F4D8 | $F4D8 | $F4F1 | $F4F1 |
| ad_play | $E03C | $F662 | $F5BC | $F5C6 | $F5C6 | $F5DF | $F5DF |
| ad_cplay | $E03F | $F6B1 | $F615 | $F61F | $F61F | $F638 | $F638 |
| ad_stop | $E042 | $F756 | $F6B7 | $F6C1 | $F6C1 | $F6DA | $F6DA |
| ad_stat | $E045 | $F761 | $F6D1 | $F6DB | $F6DB | $F6F4 | $F6F4 |
| bm_format | $E048 | $F7A2 | $F84E | $F858 | $F858 | $F871 | $F871 |
| bm_free | $E04B | $F802 | $F8AE | $F8B8 | $F8B8 | $F8D1 | $F8D1 |
| bm_read | $E04E | $F827 | $F8D9 | $F8E3 | $F8E3 | $F8FC | $F8FC |
| bm_write | $E051 | $F899 | $F94B | $F955 | $F955 | $F96E | $F96E |
| bm_delete | $E054 | $F951 | $FA10 | $FA1A | $FA1A | $FA33 | $FA33 |
| bm_files | $E057 | $F9A9 | $FA68 | $FA72 | $FA72 | $FA8B | $FA8B |
| ex_getver | $E05A | $F0BD | $F02D | $F02D | $F02D | $F046 | $F046 |
| ex_setvec | $E05D | $F0C4 | $F034 | $F034 | $F034 | $F04D | $F04D |
| ex_getfnt | $E060 | $F1A3 | $F122 | $F124 | $F124 | $F13D | $F13D |
| ex_joysns | $E063 | $E531 | $E49A | $E49A | $E49A | $E4B3 | $E4B3 |
| ex_joyrep | $E066 | $E171 | $E175 | $E175 | $E175 | $E18E | $E18E |
| ex_scrsiz | $E069 | $E2FC | $E267 | $E267 | $E267 | $E280 | $E280 |
| ex_dotmod | $E06C | $E307 | $E272 | $E272 | $E272 | $E28B | $E28B |
| ex_scrmod | $E06F | $E333 | $E29D | $E29D | $E29D | $E2B6 | $E2B6 |
| ex_imode | $E072 | $E418 | $E382 | $E382 | $E382 | $E39B | $E39B |
| ex_vmode | $E075 | $E427 | $E391 | $E391 | $E391 | $E3AA | $E3AA |
| ex_hmode | $E078 | $E43A | $E3A4 | $E3A4 | $E3A4 | $E3BD | $E3BD |
| ex_vsync | $E07B | $E44B | $E3B5 | $E3B5 | $E3B5 | $E3CE | $E3CE |
| ex_rcron | $E07E | $E45E | $E3C7 | $E3C7 | $E3C7 | $E3E0 | $E3E0 |
| ex_rcroff | $E081 | $E462 | $E3CB | $E3CB | $E3CB | $E3E4 | $E3E4 |
| ex_irqon | $E084 | $E466 | $E3CF | $E3CF | $E3CF | $E3E8 | $E3E8 |
| ex_irqoff | $E087 | $E46A | $E3D3 | $E3D3 | $E3D3 | $E3EC | $E3EC |
| ex_bgon | $E08A | $E479 | $E3E2 | $E3E2 | $E3E2 | $E3FB | $E3FB |
| ex_bgoff | $E08D | $E47C | $E3E5 | $E3E5 | $E3E5 | $E3FE | $E3FE |
| ex_spron | $E090 | $E47F | $E3E8 | $E3E8 | $E3E8 | $E401 | $E401 |
| ex_sproff | $E093 | $E482 | $E3EB | $E3EB | $E3EB | $E404 | $E404 |
| ex_dspon | $E096 | $E485 | $E3EE | $E3EE | $E3EE | $E407 | $E407 |
| ex_dspoff | $E099 | $E48A | $E3F3 | $E3F3 | $E3F3 | $E40C | $E40C |
| ex_dmamod | $E09C | $E48F | $E3F8 | $E3F8 | $E3F8 | $E411 | $E411 |
| ex_sprdma | $E09F | $E4A2 | $E40B | $E40B | $E40B | $E424 | $E424 |
| ex_satclr | $E0A2 | $E671 | $E5DA | $E5DA | $E5DA | $E5F3 | $E5F3 |
| ex_sprput | $E0A5 | $E6D3 | $E63C | $E63C | $E63C | $E655 | $E655 |
| ex_setrcr | $E0A8 | $E4B6 | $E41F | $E41F | $E41F | $E438 | $E438 |
| ex_setred | $E0AB | $E4C6 | $E42F | $E42F | $E42F | $E448 | $E448 |
| ex_setwrt | $E0AE | $E4DD | $E446 | $E446 | $E446 | $E45F | $E45F |
| ex_setdma | $E0B1 | $E4F4 | $E45D | $E45D | $E45D | $E476 | $E476 |
| ex_binbcd | $E0B4 | $E6B8 | $E621 | $E621 | $E621 | $E63A | $E63A |
| ex_bcdbin | $E0B7 | $E697 | $E600 | $E600 | $E600 | $E619 | $E619 |
| ex_rnd | $E0BA | $E707 | $E67E | $E67E | $E67E | $E697 | $E697 |
| ma_mul8u | $E0BD | $FD22 | $FDBC | $FDC6 | $FDC6 | $FDDF | $FDDF |
| ma_mul8s | $E0C0 | $FCF2 | $FDB5 | $FDBF | $FDBF | $FDD8 | $FDD8 |
| ma_mul16u | $E0C3 | $FD3F | $FDCA | $FDD4 | $FDD4 | $FDED | $FDED |
| ma_div16s | $E0C6 | $FD70 | $FDD8 | $FDE2 | $FDE2 | $FDFB | $FDFB |
| ma_div16u | $E0C9 | $FDC3 | $FDDF | $FDE9 | $FDE9 | $FE02 | $FE02 |
| ma_sqrt | $E0CC | $FE0B | $FDE6 | $FDF0 | $FDF0 | $FE09 | $FE09 |
| ma_sin | $E0CF | $FE5C | $FDF4 | $FDFE | $FDFE | $FE17 | $FE17 |
| ma_cos | $E0D2 | $FE66 | $FDED | $FDF7 | $FDF7 | $FE10 | $FE10 |
| ma_atni | $E0D5 | $FE6E | $FDFB | $FE05 | $FE05 | $FE1E | $FE1E |
| psg_bios | $E0D8 | $FF14 | $FE02 | $FE0C | $FE0C | $FE25 | $FE25 |
| grp_bios | $E0DB | $FF5B | $FE4D | $FE57 | $FE57 | $FE70 | $FE70 |
| key_bios / ex_memopen | $E0DE | $FCCA | $E175 | $E175 | $FE92 | $E18E | $FEAB |
| psg_driver | $E0E1 | $E759 | $E6CF | $E6CF | $E6CF | $E6E8 | $E6E8 |
| ex_colorcmd | $E0E4 | $E5A0 | $E509 | $E509 | $E509 | $E522 | $E522 |
| ad_stream_start | $E0E7 | N/A | $F726 | $F730 | $F730 | $F749 | $F749 |
| ad_stream_poll | $E0EA | N/A | $F762 | $F76C | $F76C | $F785 | $F785 |
| ma_mul16s | $E0ED | N/A | $FDC3 | $FDCD | $FDCD | $FDE6 | $FDE6 |
| ma_div8u | $E0F0 | N/A | $FDD1 | $FDDB | $FDDB | $FDF4 | $FDF4 |
    Post Tags
      PC Engine
