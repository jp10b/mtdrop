# M0 – dowód, że chip MT7921 w tym laptopie umie AirDrop (Linux z pendrive'a)

> Instrukcja dla maintainera (po polsku, wyjątek od zasady „docs po angielsku” – CLAUDE.md §11.6).
> Cel M0: **zdjęcie z iPhone'a dociera na ten laptop pod Linuksem**, przy użyciu gotowego projektu `jedbillyb/airdrop-mt7921`.
> Jeśli M0 się nie uda – zatrzymujemy się i przemyślimy cały projekt, zanim napiszemy linijkę sterownika.
> Czas: ok. 1–1,5 h. Windows i dysk wewnętrzny zostają nietknięte (poza jednym nowym folderem na wyniki).

---

## 0. Dlaczego nie maszyna wirtualna i jak nie stracić sesji z Claude

**VM odpada.** VMware Workstation nie umie przekazać do maszyny wirtualnej wbudowanej karty PCIe (Wi-Fi). Linux w VM widziałby wirtualną kartę sieciową, a nie MT7921 – a M0 ma udowodnić, że *ten konkretny chip* działa. Hyper-V z przekazywaniem urządzeń (DDA) nie istnieje w Windows 11 Home. Więc: prawdziwy Linux, uruchomiony z pendrive'a.

**Sesja z Claude nie ginie:**
- Wszystko, co ustaliliśmy, jest w plikach: `CLAUDE.md`, `docs/RESEARCH.md`, ta instrukcja.
- Po powrocie do Windows otwórz terminal w `C:\Users\desktop\Desktop\airdrop` i uruchom `claude --continue` – wrócisz do tej samej rozmowy.
- Instrukcję możesz czytać pod Linuksem (jest na dysku Windows, krok 6) albo na drugim komputerze.
- Opcjonalnie możesz zainstalować Claude Code także pod Linuksem (krok 7) – to będzie nowa rozmowa, ale przeczyta `CLAUDE.md` i tę instrukcję, więc będzie wiedział, o co chodzi.

---

## 1. Przygotowanie pendrive'a (na Windows)

1. Pobierz **Ubuntu 26.04 LTS Desktop** (plik `.iso`, ok. 6 GB) ze strony https://ubuntu.com/download/desktop.
   - Dlaczego ta wersja: nowsze jądra Linuksa wyłączyły dla MT7921 tryb „active monitor”, od którego zależy cały test (szczegóły: `docs/RESEARCH.md` §0 pkt 4). Ubuntu 26.10 ma za nowe jądro. **ZAŁOŻENIE:** 26.04 ma jądro 7.0, w którym tryb jeszcze działa – sprawdzimy to w kroku 4, zanim cokolwiek zainstalujesz.
2. Pobierz **Rufus** z https://rufus.ie (wersja przenośna wystarczy).
3. Włóż pendrive 64 GB. **Wszystko na nim zostanie skasowane.**
4. W Rufusie:
   - Urządzenie: Twój pendrive (sprawdź pojemność, żeby nie wybrać innego dysku!)
   - Wybór bootowania: plik ISO Ubuntu
   - **Rozmiar partycji trwałej (Persistent partition size): ok. 20 GB** – dzięki temu instalacje i wyniki przetrwają restart. (Jeśli tej opcji nie ma lub później coś z nią nie działa – nic straconego, test da się zrobić w jednej sesji bez niej.)
   - Schemat partycji: **GPT**, system docelowy: **UEFI**
   - START. Jeśli zapyta o tryb ISO/DD – wybierz **ISO**.

## 2. BIOS: włącz menu startowe (jednorazowo)

Na laptopach Acer menu wyboru dysku (F12) bywa domyślnie wyłączone.

1. Uruchom ponownie komputer i wciskaj **F2** zaraz po włączeniu → wejdziesz do BIOS-u.
2. Zakładka **Main** → **F12 Boot Menu** → ustaw **Enabled**.
3. Secure Boot masz już wyłączony – nic więcej nie zmieniaj.
4. **F10** → zapisz i wyjdź.

## 3. Start z pendrive'a

1. W Windows wybierz **Uruchom ponownie** (nie „Zamknij” – restart zawsze w pełni zamyka Windows, dzięki czemu Linux będzie mógł zapisać wyniki na dysku C:).
2. Wciskaj **F12** przy starcie → wybierz pendrive (USB).
3. W menu Ubuntu wybierz pierwszą opcję (Try or Install Ubuntu).
4. W instalatorze wybierz język i kliknij **„Wypróbuj Ubuntu”**.
   **Nie klikaj „Zainstaluj Ubuntu”** – nie instalujemy niczego na dysk laptopa.

## 4. Pierwsze sprawdzenie (5 minut, DECYDUJĄCE)

1. Otwórz terminal: **Ctrl+Alt+T**.
2. Wpisz:
   ```sh
   uname -r
   iw list | grep -i "active monitor"
   lspci -nnk -d 14c3:
   ```
3. Oczekiwany wynik:
   - druga komenda wypisuje **`Device supports active monitor ...`**
   - trzecia pokazuje `[14c3:7961]` i `Kernel driver in use: mt7921e`
4. **Jeśli „active monitor” się NIE pojawi – STOP.** Zapisz wynik `uname -r` (zdjęcie ekranu telefonem wystarczy), wróć do Windows i powiedz mi. Plan B to inna dystrybucja lub inne jądro (np. Void Linux z jądrem 6.18, na którym działał autor `airdrop-mt7921`).

Jeśli `iw` nie istnieje: `sudo apt install -y iw` (najpierw krok 5 – internet).

## 5. Internet (do instalacji pakietów)

Połącz się z routerem przez Wi-Fi normalnie (ikonka w prawym górnym rogu). Wi-Fi jest potrzebne tylko do instalacji – podczas samego testu Wi-Fi zniknie na ok. 2–3 minuty i wróci samo.

## 6. Otwórz dysk Windows i uruchom skrypt instalacyjny

1. Otwórz **Pliki** (menedżer plików). Po lewej kliknij dysk Windows (ok. pojemności Twojego C:). Ubuntu go zamontuje.
2. W terminalu sprawdź, gdzie się zamontował:
   ```sh
   ls /media/$USER/ /run/media/$USER/ 2>/dev/null
   ```
   Uwaga z testu 2026-11-09: sesja Live Ubuntu 26.04 montuje dysk w `/run/media/$USER/…`, nie w `/media/$USER/…`. W dalszych poleceniach użyj tej ścieżki, która u Ciebie istnieje.

   Zobaczysz nazwę (np. `Acer` albo długi identyfikator). Dalej nazywam ją `<WIN>`.
3. Ta instrukcja jest pod: `/media/$USER/<WIN>/Users/desktop/Desktop/airdrop/docs/M0-GUIDE.pl.md` – możesz ją otworzyć w edytorze tekstu albo w Firefoksie.
4. Uruchom przygotowanie (pobiera pakiety, buduje OWL i poprawionego OpenDropa – ok. 10 minut):
   ```sh
   bash "/media/$USER/<WIN>/Users/desktop/Desktop/airdrop/tools/m0/m0-setup.sh"
   ```
   - Poprosi o hasło sudo – w sesji „Wypróbuj Ubuntu” hasło jest puste, po prostu **Enter**.
   - Skrypt najpierw jeszcze raz sprawdza „active monitor”. Jeśli wypisze STOP – patrz krok 4.
   - Instalator `airdrop-mt7921` zapyta o dodanie reguły sudoers (pozwala jego pomocnikowi działać bez hasła). To system tymczasowy na pendrivie – odpowiedz **y**.
   - Na końcu powinno być `==> Done` i `Setup finished`.
   - Jeśli coś się wysypie: zrób zdjęcie ostatnich ~20 linii i przejdź do kroku 11.

> Jeśli dysk Windows zamontował się **tylko do odczytu** (błąd „read-only file system” później w kroku 10): Windows ma włączone „szybkie uruchamianie”. Wtedy wyniki zapiszemy później – patrz krok 10, wariant B.

## 7. (Opcjonalnie) Claude Code pod Linuksem

Jeśli chcesz mieć pomoc na żywo podczas testu:
```sh
curl -fsSL https://claude.ai/install.sh | bash
cd "/media/$USER/<WIN>/Users/desktop/Desktop/airdrop"
~/.local/bin/claude
```
Zaloguj się w przeglądarce. Napisz mu np.: „Robię M0 wg docs/M0-GUIDE.pl.md, jestem na kroku 8”. Uwaga: w trakcie testu (krok 9) Wi-Fi zniknie na kilka minut, więc Claude będzie wtedy offline – to normalne.

## 8. Przygotuj iPhone'a

1. **Hotspot osobisty: WYŁĄCZONY** (może zajmować radio Wi-Fi telefonu).
2. Wi-Fi i Bluetooth: **włączone** (Wi-Fi nie musi być połączone z siecią).
3. Ustawienia → Ogólne → AirDrop → **Wszyscy przez 10 minut**. (Po 10 minutach iOS sam to wyłącza bez ostrzeżenia – przy kolejnej próbie ustaw ponownie.)
4. Przygotuj w Zdjęciach jedno zwykłe zdjęcie (nie wideo, nie Live Photo – na początek najprostszy przypadek).
5. Telefon odblokowany, blisko laptopa (do 1 m).
6. Zapisz wersję iOS (26.5.2 – nie aktualizuj).

## 9. Test odbioru

Potrzebujesz dwóch okien terminala (Ctrl+Alt+T dwa razy).

**Terminal 1 – start odbioru** (180 s okna odbioru, region Polska, interfejs potwierdzający ramki):
```sh
cd ~/m0/airdrop-mt7921
REG=PL ACTIVE=1 RECV_TIME=180 SSLKEYLOGFILE=~/m0/sslkeys.log ./airdrop.sh receive
```
- Wi-Fi się rozłączy – to oczekiwane.
- Skrypt przechodzi przez warstwy (`### layer 1`, `layer 0` – szukanie kanału, `layer 2`, …). Na etapie szukania kanału **już otwórz arkusz udostępniania na iPhonie** (patrz niżej) – bez otwartego arkusza telefon nie nadaje nic po AWDL.

**Terminal 2 – opcjonalne nagranie surowych ramek AWDL** (gdy w terminalu 1 pojawi się `layer 0` albo dalej):
```sh
sudo timeout 240 tcpdump -i mon0 -w ~/m0/awdl-raw.pcap "wlan addr3 00:25:00:ff:94:73"
```

**Na iPhonie:**
1. Zdjęcia → wybrane zdjęcie → **Udostępnij** → kafelek **AirDrop**. Trzymaj ten ekran otwarty.
2. Gdy terminal 1 wypisze `### layer 4: advertising as an AirDrop receiver` – czekaj **do 1 minuty**, aż laptop pojawi się na liście.
3. Stuknij laptopa. Jeśli na laptopie pojawi się pytanie o akceptację – zaakceptuj.
4. Spodziewane tempo: ok. 45–67 kB/s, czyli zwykłe zdjęcie (2–4 MB) to **ok. 1 minuta**. Nie przerywaj.

Po zakończeniu okna (180 s) skrypt sam przywraca Wi-Fi (do ok. 30 s). Jeśli nie wróci: `sudo systemctl start NetworkManager`.

**Zapisz krótko, co widziałeś** (przyda się bardzo):
```sh
nano ~/m0/notes.txt
```
Np.: „laptop pojawił się po ~40 s, wysyłanie trwało ~1 min, zdjęcie OK” albo „laptop nie pojawił się wcale”. Zapis: Ctrl+O, Enter, Ctrl+X.

**Sukces M0 = zdjęcie jest w `~/Downloads`** i otwiera się (`xdg-open ~/Downloads`).

## 10. Zbierz wyniki i zapisz je na dysku Windows

**Wariant A (normalny):**
```sh
bash "/media/$USER/<WIN>/Users/desktop/Desktop/airdrop/tools/m0/m0-collect.sh" "/media/$USER/<WIN>"
```
Skrypt tworzy raport (`m0-report.txt`: wersje jądra, sterownika, firmware, wynik „active monitor”) i kopiuje wszystko do:
`C:\Users\desktop\mtdrop-private\m0-<data>\`

To folder **poza repozytorium**. Pliki `.pcap`, logi i `sslkeys.log` zawierają nazwę i adresy Twojego telefonu – **nigdy ich nie commitujemy ani nie wysyłamy nikomu**. Ja przeczytam je lokalnie i do repo trafią tylko zanonimizowane wnioski.

**Wariant B (dysk Windows tylko do odczytu):**
- Jeśli masz persistence (krok 1) – uruchom skrypt bez argumentu: `bash ".../m0-collect.sh"`, wyniki zostaną w `~/m0` na pendrivie. Po powrocie do Windows wyłącz „szybkie uruchamianie” (Panel sterowania → Opcje zasilania → Wybierz działanie przycisków zasilania → odznacz „Włącz szybkie uruchamianie”), wróć do Linuksa i zrób wariant A.
- Bez persistence – nie restartuj, zapisz `~/m0/m0-report.txt` na drugi komputer (np. przez przeglądarkę na swoją pocztę do siebie samego **tylko raport**, nie pcapy) i powiedz mi.

## 11. Gdy coś nie działa

Do 3 prób, potem wracamy do Windows i analizujemy wyniki. Przed każdą próbą:
- AirDrop na iPhonie nadal na „Wszyscy przez 10 minut”? (najczęstsza przyczyna)
- Arkusz udostępniania otwarty i telefon odblokowany przez cały czas?
- Daj telefonowi pełną minutę.

Warianty do kolejnych prób (zapisz w `notes.txt`, który próbowałeś):
1. Bez stałego kanału startowego: `REG=PL ACTIVE=1 RECV_TIME=180 CHAN= SSLKEYLOGFILE=~/m0/sslkeys.log ./airdrop.sh receive`
2. Kanał 6: jak wyżej, ale `CHAN=6`.

Nawet nieudana próba jest cenna – logi w `~/m0/airdrop-mt7921/runs/` mówią, na której warstwie się zatrzymało. **Zawsze wykonaj krok 10.**

## 12. Powrót do Windows

1. W Ubuntu: menu w prawym górnym rogu → Wyłącz/Uruchom ponownie → wyjmij pendrive, gdy ekran zgaśnie.
2. Windows uruchomi się normalnie.
3. Terminal → `cd C:\Users\desktop\Desktop\airdrop` → `claude --continue`.
4. Napisz: **„M0 zrobione”** (albo „M0 nie wyszło na kroku X”). Przeczytam raport i logi z `mtdrop-private`, wpiszę wyniki do `docs/FINDINGS.md` (bez danych osobowych) i zdecydujemy o dalszych krokach.

---

## Co M0 ma nam dać (dla porządku)

| Co | Skąd | Po co |
|---|---|---|
| Czy odbiór działa na tym laptopie | `notes.txt`, `~/Downloads` | Kryterium akceptacji M0 – decyzja „jedziemy dalej / stop” |
| Wersja jądra, sterownika, „active monitor” | `m0-report.txt` | Powtarzalność, porównanie z `airdrop-mt7921` |
| Pliki i sumy kontrolne firmware z Linuksa | `m0-report.txt` | Porównanie z firmware z paczki MediaTek na Windows (RESEARCH §5) |
| Przebieg warstw, kanał, tempo | `runs/airdrop-*/` | Punkt odniesienia dla naszego sterownika (M3–M5) |
| Surowe ramki AWDL | `awdl-raw.pcap` | Kanały i sekwencja kanałów Twojego iPhone'a na iOS 26.5.2 (stały kanał vs skakanie – RESEARCH §9 pkt 2) |
| Nagłówki `/Ask`, `/Upload`, klucze TLS | `runs/*/receive.log`, `sslkeys.log` | Fixtures do testów warstwy protokołu (M2) – po anonimizacji |

Bezpieczeństwo: nic nie jest instalowane na dysk laptopa, nie zmieniamy sterowników Windows ani BIOS-u (poza włączeniem menu F12). Jedyny zapis na dysku Windows to nowy folder `C:\Users\desktop\mtdrop-private\`.
