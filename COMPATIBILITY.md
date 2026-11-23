# Compatibility

How far each title that was tried gets when it is started. The ID is the title ID the
launcher shows next to the name.

| State        | Meaning                                                                  |
| ------------ | ------------------------------------------------------------------------ |
| **Playable** | Can be played from start to end without problems that stop it            |
| **In-game**  | Gets into gameplay, with problems that keep it from being played through |
| **Menu**     | Reaches its main menu or title screen with the right picture             |
| **Intro**    | Shows its logos, loading or first screens, and gets no further           |
| **Boots**    | Starts and runs, with no picture or a wrong one                          |
| **Nothing**  | Ends with an error before it shows anything                              |
| **Untested** | In the library, not run yet                                              |

States come from short runs (under a minute, nobody at the gamepad), so a title may get
further than its state says. Logs of a run are in `%APPDATA%\xvalkyrie\logs`.

## Xbox One

| Title                                    | ID         | State        | Notes                                                                                                             |
| ---------------------------------------- | ---------- | ------------ | ----------------------------------------------------------------------------------------------------------------- |
| DRAGON BALL FighterZ                     | `00F385F6` | **Menu**     | Unreal Engine 4. The lobby, a 3D scene, with the right picture                                                    |
| Forza Motorsport 6 Demo                  | `132D50F5` | **Menu**     | Direct3D 11.X. Main menu after about a minute; its text and logo are right, the background behind them is striped |
| Minecraft: Xbox One Edition              | `149E11AE` | **Menu**     | Direct3D 11.X. Main menu with the right picture                                                                   |
| Terraria                                 | `1738CA2A` | **Playable** | Main menu with the right picture; writes its save                                                                 |
| Puyo Puyo Champions                      | `62B3A350` | **Menu**     | Direct3D 11.X. The autosave notice, waiting for A; reads the gamepad                                              |
| Crash Bandicoot N. Sane Trilogy          | `044D0F74` | **Boots**    | Runs without a frame in 40 s; logs `app:/archives/update.pak not found`                                           |
| Mega Man 11                              | `1F337CFB` | **Boots**    | Direct3D 11.X. Presents frames, the picture is wrong: a few stray triangles on black                              |
| Overcooked                               | `145207D1` | **Boots**    | Unity. One frame, then its Mono runtime stops on a thread it does not know                                        |
| Rock Band 4                              | `2DBB840F` | **Boots**    | Direct3D 11.X. Presents frames, the picture is black; its Xbox Live library throws on start                       |
| SPACE JAM: A NEW LEGACY - THE GAME       | `622B018E` | **Boots**    | GDK title on Direct3D 12. Creates its device, no frame                                                            |
| LEGO CITY UNDERCOVER                     | `27D78EC2` | **Nothing**  | Reads the gamepad, then calls method 17 of `Windows.Xbox.Input.Controller`, which is not implemented              |
| Minecraft (Bedrock)                      | `6CFA0C1E` | **Nothing**  | Direct3D 12. Faults right after its device is created                                                             |
| Rocket League                            | `214E91AC` | **Nothing**  | Unreal Engine 3 gives up with `Invalid DefaultMaterialName`                                                       |
| 1001 Spikes                              | `6B2E4F1B` | **Boots**    |                                                                                                                   |
| 7 Days to Die                            | `039D30F6` | **Nothing**  |                                                                                                                   |
| A KING'S TALE: FINAL FANTASY XV          | `05401907` | **Intro**    |                                                                                                                   |
| Cyberpunk 2077                           | `0D42AD14` | **Nothing**  |                                                                                                                   |
| Dance Central Spotlight                  | `295F3C44` | **Nothing**  |                                                                                                                   |
| DARK SOULS: REMASTERED NETWORK TEST VER. | `27EDC966` | **Menu**     |                                                                                                                   |
| FIFA 15                                  | `64B02253` | **Boots**    |                                                                                                                   |
| FRU                                      | `3C646A12` | **Nothing**  |                                                                                                                   |
| FUSER Demo                               | `61BEB2FD` | **Nothing**  |                                                                                                                   |
| Just Dance 2020                          | `13E02891` | **Nothing**  |                                                                                                                   |
| Minecraft Story Mode                     | `781521BD` | **Boots**    |                                                                                                                   |
| Peggle 2                                 | `32966611` | **Nothing**  |                                                                                                                   |
| Pixel Piracy                             | `30D1E0F7` | **Boots**    |                                                                                                                   |
| Project Spark                            | `60D19E14` | **Nothing**  |                                                                                                                   |
| Promenade (Demo)                         | `6DBB4B4C` | **Boots**    |                                                                                                                   |
| Rayman Legends                           | `27BDEA18` | **Nothing**  |                                                                                                                   |
| Saints Row Boss Factory                  | `65D9BC13` | **Nothing**  |                                                                                                                   |
| Seasons after Fall                       | `30AD5A99` | **Boots**    |                                                                                                                   |
| Shadow of the Tomb Raider                | `36773C0D` | **Nothing**  |                                                                                                                   |
| Slender - The Arrival                    | `458E04D7` | **Boots**    |                                                                                                                   |
| SONIC FORCES                             | `07B5597E` | **Nothing**  |                                                                                                                   |
| Sonic Mania                              | `34BA1DE0` | **Playable** |                                                                                                                   |
| Stardew Valley                           | `1240B5DA` | **Nothing**  |                                                                                                                   |
| Titanfall                                | `4D046B58` | **Nothing**  |                                                                                                                   |
| Trackmania                               | `700107ED` | **Nothing**  |                                                                                                                   |
| UNDERTALE                                | `62F67ADD` | **Nothing**  |                                                                                                                   |

## Xbox Series X|S

| Title                                             | ID         | State        | Notes                                                                                                       |
| ------------------------------------------------- | ---------- | ------------ | ----------------------------------------------------------------------------------------------------------- |
| Minecraft Preview                                 | `639AA8DD` | **Playable** | Main menu with the right picture                                                                            |
| Unicorn Overlord                                  | `6897AC80` | **Playable** |                                                                                                             |
| Fortnite                                          | `0FF4B5BD` | **Menu**     | Unreal Engine. Loading screen with the right picture, at "connecting"; it needs its servers to go on        |
| Psychonauts 2                                     | `68076383` | **Intro**    | Unreal Engine 4. First frames, then the host drops the Direct3D 12 device                                   |
| XDefiant                                          | `799B973E` | **Boots**    | First frames, then the host drops the Direct3D 12 device (`0x887a0005`). 1966 of its 2004 shaders translate |
| Concept Destruction                               | `72A634A4` | **Nothing**  |                                                                                                             |
| Dynacat                                           | `6322C08F` | **Nothing**  |                                                                                                             |
| The Matrix Awakens: An Unreal Engine 5 Experience | `7D3CD3FE` | **Boots**    |                                                                                                             |

The 40 public GDK samples that build for Series all draw their picture; they are samples, not
games, and are not listed.

## Reporting a title

Open an issue with the title's name and ID, the version of XValkyrie, and the log of the run.
Say what is on screen when it stops.
