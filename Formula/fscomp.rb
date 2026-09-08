class Fscomp < Formula
  desc "Transparent filesystem compression manager for macOS"
  homepage "https://github.com/ltmerletti/fscomp"
  url "https://github.com/ltmerletti/fscomp/archive/refs/tags/v1.0.0.tar.gz"
  sha256 "8f666e5638a3609fe7729adf33a39b74b80a280dd9c944a2777abf0ee63753f6"
  license "MIT"
  head "https://github.com/ltmerletti/fscomp.git", branch: "main"

  depends_on :macos
  depends_on "afsctool"

  def install
    system "make"
    system "make", "install", "PREFIX=#{prefix}"
  end

  test do
    assert_match version.to_s, shell_output("#{bin}/fscomp --version")
  end
end
