# load-balancer

## Cerinte minime

- load balancer-ul acceptă conexiuni TCP pe o adresă și un port din configurație și trimite fiecare conexiune către backend
- lista de backend-uri se citește la pornire dintr-un fișier de configurare, verificare de configurație corectă
- algoritmul de load balancing este ales din config
- verifică starea backend-urilor periodic
- dacă un backend a căzut sau conexiunea eșuează atunci conexiunile noi merg automat la un backend sănătos
- un utilitar de administrare vorbește cu master-ul printr-un socket și poate afișa backend-urile și informațiile utile despre acestea. de aici se poate dezactiva și reactiva un backend
- la 'SIGHUP' masterul recitește configurația. dacă noua configurație e greșită, semnalează acest lucru și păstrează configurația veche
- masterul repornește un worker care a murit neașteptat, cu timeout ca să nu pornească procese la nesfârșit
- numărul de procese worker se setează în config, toate ascultă pe același port și folosesc același tabel de backenduri
