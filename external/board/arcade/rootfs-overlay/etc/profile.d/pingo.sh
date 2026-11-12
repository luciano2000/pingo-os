# Raiz somente leitura: caches (ex.: shaders do Mesa) vão para a partição ROMS
if mountpoint -q /mnt/sd1; then
	export XDG_CACHE_HOME=/mnt/sd1/.system/cache
fi
