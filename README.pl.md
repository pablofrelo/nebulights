# nebulights

Wygaszacz ekranu dla kompozytorów Wayland w duchu *Voodoo Lights*
Sergia Duarte, wygaszacza z czasów kart 3dfx z początku lat 2000.

![nebulights](docs/demo.gif)

*[English](README.md)*

Obserwator siedzi nieruchomo w środku masy i tylko obraca spojrzenie.
Wokół niego krążą po orbitach świetliste obiekty, każdy ciągnąc za sobą
wstęgę, która gaśnie i rozpływa się w mgłę. Część leci parami i trójkami,
krążąc wokół siebie po śrubie. Z jednych sypią się iskry, z innych para.
Co jakiś czas któryś detonuje i odradza się gdzie indziej, zawsze poza
kadrem.

Wymaga kompozytora z obsługą `wlr-layer-shell-unstable-v1`
i `ext-idle-notify-v1`. Sprawdzony na niri i KDE Plasma; Hyprland, Sway
i river też powinny działać.

## Instalacja

Arch Linux (AUR):

    paru -S nebulights

Ze źródeł:

    # Arch: pacman -S --needed base-devel wayland libglvnd
    make
    sudo make install          # -> /usr/local/bin/nebulights

## Użycie

    nebulights                 # start po 5 min bezczynności
    nebulights -t 120          # ...po 2 min
    nebulights --now           # od razu, wyjście po klawiszu
    nebulights -I              # ignoruj blokady bezczynności

Klawisz albo ruch myszy kończy pokaz. **To nie jest blokada ekranu
i niczego nie chroni.**

Wygaszacz musi wystartować, zanim kompozytor zgasi monitory albo
zablokuje ekran, więc te czasy ustaw dłuższe niż `-t`.

Usługa systemd (niri, Plasma i każda sesja z `graphical-session.target`):

    systemctl --user enable --now nebulights.service

niri:

    spawn-at-startup "nebulights" "-t" "300"

Hyprland:

    exec-once = nebulights -t 300

## Konfiguracja

`~/.config/nebulights.conf`, format `klucz wartość`, `#` to komentarz.
Opis wszystkich kluczy w [`nebulights.conf.example`](nebulights.conf.example).

    palette 2      # 0 = tęcza, 1 = gruvbox, 2 = nostromo

Szczegóły (wydajność, blokady przy filmach, miernik klatek
`NEBULIGHTS_STATS=1`) są w [angielskim README](README.md).

## O kodzie

Kod nebulights napisało AI (Claude od Anthropic) pod moim kierunkiem:
ja wyznaczałem cel i wygląd, testowałem na swoim sprzęcie i zgłaszałem,
co nie gra, runda po rundzie. Sam nie napisałem ani linijki i chcę, żeby
to było jasne od początku.

## Licencja

[MIT](LICENSE)
