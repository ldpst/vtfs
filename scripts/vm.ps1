param(
    [ValidateSet('Setup', 'Build', 'Test', 'Mount', 'Unmount', 'Status')]
    [string]$Action = 'Test',
    [ValidatePattern('^[a-zA-Z][a-zA-Z0-9-]*$')]
    [string]$Instance = 'vtfs'
)

$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent $PSScriptRoot

function Invoke-Multipass {
    & multipass @args
    if ($LASTEXITCODE -ne 0) {
        throw "Multipass failed with exit code $LASTEXITCODE"
    }
}

if ($Action -eq 'Setup') {
    $inventory = Invoke-Multipass list --format json | ConvertFrom-Json
    $vm = $inventory.list | Where-Object name -EQ $Instance
    if (-not $vm) {
        Invoke-Multipass launch 24.04 --name $Instance --cpus 2 --memory 2G --disk 10G --cloud-init (Join-Path $project 'cloud-init.yaml')
    } elseif ($vm.state -ne 'Running') {
        Invoke-Multipass start $Instance
    }
    Invoke-Multipass exec $Instance '--' cloud-init status --wait
    Invoke-Multipass exec $Instance '--' sh -c 'if ! command -v gcc >/dev/null || ! command -v make >/dev/null || ! command -v python3 >/dev/null || [ ! -d /lib/modules/$(uname -r)/build ]; then sudo apt-get update && sudo apt-get install -y build-essential python3 linux-headers-$(uname -r); fi'
}

if ($Action -in @('Setup', 'Build', 'Test')) {
    Invoke-Multipass exec $Instance '--' mkdir -p /home/ubuntu/vtfs2/source /home/ubuntu/vtfs2/tests /home/ubuntu/vtfs2/scripts
    $files = @('Makefile', 'source/vtfs.h', 'source/vtfs.c', 'source/inode.c', 'source/dir.c', 'source/file.c', 'scripts/test.sh', 'scripts/mount.sh', 'tests/test_vtfs.py')
    foreach ($file in $files) {
        Invoke-Multipass transfer (Join-Path $project $file) "${Instance}:/home/ubuntu/vtfs2/$file"
    }
    Invoke-Multipass exec $Instance '--' make -C /home/ubuntu/vtfs2
}

switch ($Action) {
    'Test' {
        Invoke-Multipass exec $Instance '--' sh -c 'cd /home/ubuntu/vtfs2 && sudo sh scripts/test.sh'
    }
    'Mount' {
        Invoke-Multipass exec $Instance '--' sh -c 'cd /home/ubuntu/vtfs2 && sudo sh scripts/mount.sh'
    }
    'Unmount' {
        Invoke-Multipass exec $Instance '--' sudo umount /mnt/vtfs
        Invoke-Multipass exec $Instance '--' sudo rmmod vtfs
    }
    'Status' {
        Invoke-Multipass info $Instance
        Invoke-Multipass exec $Instance '--' sh -c 'uname -r; findmnt -t vtfs || true; if [ -d /sys/module/vtfs ]; then echo VTFS-loaded; else echo VTFS-unloaded; fi'
    }
}
