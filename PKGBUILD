pkgname=display-audio-bridge
pkgver=3.0.0
pkgrel=1
pkgdesc="PipeWire volume controls backed by DDC/CI display hardware"
arch=('x86_64')
url="https://github.com/satorusaka/display-audio-bridge"
license=('MIT')
depends=('ddcutil' 'libpulse' 'pipewire-pulse' 'python' 'python-gobject'
         'gtk4' 'libadwaita' 'libnotify')
makedepends=('git')
source=("$pkgname-$pkgver.tar.gz::$url/archive/refs/tags/v$pkgver.tar.gz")
sha256sums=('SKIP')

build() {
  make -C "$pkgname-$pkgver"
}

check() {
  make -C "$pkgname-$pkgver" check
}

package() {
  cd "$pkgname-$pkgver"
  make DESTDIR="$pkgdir" PREFIX=/usr install
  install -Dm644 systemd/display-audio.service \
    "$pkgdir/usr/lib/systemd/user/display-audio.service"
  install -Dm644 data/io.github.satorusaka.DisplayAudio.Settings.desktop \
    "$pkgdir/usr/share/applications/io.github.satorusaka.DisplayAudio.Settings.desktop"
  install -Dm644 data/io.github.satorusaka.DisplayAudio.metainfo.xml \
    "$pkgdir/usr/share/metainfo/io.github.satorusaka.DisplayAudio.metainfo.xml"
  install -Dm644 LICENSE "$pkgdir/usr/share/licenses/$pkgname/LICENSE"
}
