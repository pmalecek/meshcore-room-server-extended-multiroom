# Room server: sub-rooms

Upraveno nad MeshCore v1.18.0 (commit 75b04ef). Companion firmware ani aplikace se nemění.

## Co to dělá
- Jeden room server má až 4 sub-rooms: **0 = výchozí** (nejde smazat, jen přejmenovat) a **1–3 volitelné**.
- Nový klient spadne do room 0 a dostane uvítací zprávu.
- Server si každému klientovi pamatuje, ve které room byl. Po dalším přihlášení ho vrátí tam a připomene mu to.
- Synchronizaci si server vede pro každou room zvlášť, takže se při přepínání neztrácí zprávy.
- Buffer nepřeposlaných zpráv je zvětšený na 64 (`MAX_UNSYNCED_POSTS`), je společný a každá zpráva nese značku své room.

## Příkazy pro uživatele (napíšou se jako běžná zpráva do chatu)
| Příkaz | Význam |
|---|---|
| `/rooms` (`/r`) | seznam rooms, `*` = kde jsem |
| `/join <číslo\|název>` (`/j`) | přepnutí do room |
| `/exit` (`/leave`) | návrat do výchozí room (0) |
| `/where` (`/w`) | kde jsem |
| `/help` | nápověda |

Fungují i pro hosty bez práva zápisu. Server toleruje mezery a neviditelné znaky před lomítkem i JSON-escapované `\/` (některé klienty). Zpráva začínající `/` se neuloží jako příspěvek.

## Admin CLI (sériová linka nebo vzdálená správa z companion aplikace)
| Příkaz | Význam |
|---|---|
| `room.list` | rooms s počtem zpráv (p) a klientů (c) |
| `room.add <název>` | přidá room do volného slotu 1–3 |
| `room.del <č\|název>` | smaže room i její zprávy, klienty přesune do 0 |
| `room.rename <č\|název> <nový>` | přejmenování (i room 0) |
| `room.clear <č\|název>` | smaže zprávy v room |
| `room.welcome [text\|default]` | bez parametru zobrazí, jinak nastaví uvítání (max 140 znaků) |
| `room.prefix on\|off` | prefix `[název]` před zprávami (výchozí on, jen pokud existuje víc rooms) |
| `room.post [@<č\|název>] <text>` | systémová zpráva do všech rooms, nebo do zvolené |
| `room.last` | diagnostika: poslední příspěvek (room, autor, první bajty v hexu, text) |
| `room.wake on\|off` | rozsvícení displeje při nové zprávě (výchozí on) |

Značky v uvítání: `{name}` = jméno serveru, `{room}` = aktuální room, `{rooms}` = seznam (`0=Lobby, 1=Tesla`).
Uvítání se po rozvinutí ořízne na 151 znaků (limit jedné zprávy).

Názvy rooms: 1–15 znaků, bez mezer a hranatých závorek, nesmí to být samotná číslice.
Nastavení se ukládá do `/s_rooms`.

## Displej (Heltec V3, OLED 128×64)
Tlačítko PRG přepíná obrazovky (když je displej zhasnutý, nejdřív ho rozsvítí). Po 20 s nečinnosti displej zhasne.
1. **Přehled:** jméno serveru, online klienti (aktivní za poslední hodinu), zprávy v bufferu (`42/64`), stáří poslední zprávy a v jaké room, počet rooms.
2. **Rooms:** pro každou room počet zpráv (`zp`), online klientů (`kl`) a stáří poslední zprávy (`akt`). Šipka `>` značí zprávu za poslední minutu.
3. **Rádio:** frekvence, SF, BW, CR, TX výkon.

Při nové zprávě se displej rozsvítí na obrazovce Rooms (vypnutí: `room.wake off`). Texty zpráv se na displeji nezobrazují.
Jméno serveru a názvy rooms se převádí na verzi bez diakritiky, protože font displeje české znaky nezná.

## Build flagy
- `MAX_UNSYNCED_POSTS` (výchozí 64, asi 190 B na zprávu, tj. ~12 KB RAM)
- `MAX_SUBROOMS` (výchozí 4; do 4 se stav vejde do existující struktury klienta bez nárůstu RAM)
- `SUBROOM_DEFAULT_NAME`, `SUBROOM_DEFAULT_WELCOME`, texty `SUBROOM_TXT_*` v `MyMesh.h`

## Známá omezení
- Zprávy ze všech rooms jsou v aplikaci v jednom vlákně (proto prefix `[název]`).
- Ukládají se jen admini (původní chování `ClientACL`), takže si hosté po restartu serveru room nepamatují. Zprávy jsou ale jen v RAM, takže po restartu stejně zmizí.
- Systémová zpráva do všech rooms (`room.post` bez `@`) může klient, který přepíná mezi rooms, dostat víckrát.
