### Anschlüsse ESP32:
```rot - 3v3 ------------------> 3v3
orange - klingel -----------> ESP 26
schwarz - GND --------------> GND
weiß - wählscheibe ---------> ESP 12
grau -  schwarzer taster ---> ESP 14
lila - gabel ---------------> ESP 27
gelb - reset ---------------> ESP 25

     - h bridge in 1 -------> ESP 32
     - h bridge in 2 -------> ESP 33
```
Anschlüsse Kabel außen:
```
anschluss 1 - weiß - gnd
anschluss 2 - braun - 12v
```

### URL calls
URL calls für Gabel-Aktionen so aktivieren:
  - `http://{ip}/url_after_lift` -> beim nächsten mal Gabel abheben wir die konfigurierte URL gecalled
  - `http://{ip}/url_after_hang_up` -> beim nächsten mal Gabel auflegen wir die konfigurierte URL gecalled


  - `http://{ip}/klingel_an` -> 
  - `http://{ip}/klingel_aus` ->

### WiFi config
- Option 1: während "powerup" den zweipoligen Anschluss auf der Rückseite brücken
- Option 2: innerhalb der ersten 20 Sekunden nach Start 1234 wählen ACHTUNG: geht nur, wenn schon mit einem WIFI verbunden!
Mit ESP32Config verbinden. Diese Seite aufrufen: http://192.168.4.1/