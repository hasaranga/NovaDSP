// animtool - turn a piece of a video into a 1-bit animation header for the
// 128x64 OLED.
//
// Flow: open a video -> scrub to the start and end frames and drag the crop
// box -> extract (ffmpeg gives greyscale frames already at final size) ->
// tune threshold/dither and watch it play at 25 fps -> write the .h/.c.
//
// Everything after extraction is done in Go on the cached greyscale frames,
// so moving the threshold slider re-renders instantly instead of re-running
// ffmpeg. What plays in the preview is decoded from the very bytes that get
// written to the header, so the preview cannot drift from the output.
//
// Requires ffmpeg and ffprobe on PATH. No other dependencies.
package main

import (
	"bytes"
	"embed"
	"encoding/json"
	"flag"
	"fmt"
	"image"
	"image/color"
	"image/draw"
	"image/jpeg"
	"image/png"
	"log"
	"math"
	"net"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"sync"
)

//go:embed ui.html
var uiFS embed.FS

// The scrub timeline is a fixed 25 fps grid: "frame 137" means 137/25 seconds
// into the source, in the proxy, in the range picker and in the ffmpeg range
// extraction alike. It is deliberately not the animation's frame rate - that
// is the user's choice and can change without re-decoding anything, whereas
// this grid is baked into the proxy on disk. Keeping it fixed also keeps range
// picking accurate to 40 ms no matter how slow the animation runs.
const scrubFPS = 25

// What the animation itself may run at. The panel cannot flush much past 43
// fps anyway; the low end is where the savings are, since flash cost is
// directly proportional to the rate.
const (
	minAnimFPS = 1
	maxAnimFPS = 50
)

// Refuse to proxy more than this many frames (about six minutes). Past that
// the extraction takes long enough to look hung, and no OLED animation needs
// a six minute source anyway.
const maxProxyFrames = 9000

type session struct {
	mu    sync.Mutex
	video string
	srcW  int
	srcH  int

	proxyDir string // temp dir of 25 fps scrub frames (JPEG)
	proxyN   int

	// Extraction output. gray[i] is frame i at final size; bits[i] is the same
	// frame packed the way the panel wants it; prev[i] is a PNG of bits[i] for
	// the browser. All three stay the same length.
	dir  string
	gray []*image.Gray
	bits [][]byte
	prev [][]byte

	// Which of gray[] the animation is currently made of, in order. Dropping a
	// frame takes it out of here rather than out of gray, so "restore all" is
	// free and costs no re-extraction - the greyscale is the expensive thing
	// and there is no reason to throw it away over an editing decision.
	keep []int
	outW int
	outH int
	fps  int // the rate the animation plays at, not the scrub grid

	// gen increments on every render. Preview frames are served with a long
	// cache lifetime so playback never stutters, but /api/frame?i=N is the
	// same URL across renders while the bytes change - so the client appends
	// this value to bust the cache.
	gen int
}

var sess = &session{}

// cropFrac is a rectangle stored as fractions of the frame (0..1), so the same
// values apply to the proxy-sized scrub frame and the native-resolution video.
type cropFrac struct {
	X float64 `json:"x"`
	Y float64 `json:"y"`
	W float64 `json:"w"`
	H float64 `json:"h"`
}

func (c cropFrac) rectIn(w, h int) image.Rectangle {
	if c.W <= 0.001 || c.H <= 0.001 {
		return image.Rect(0, 0, w, h)
	}
	x0 := int(c.X*float64(w) + 0.5)
	y0 := int(c.Y*float64(h) + 0.5)
	x1 := int((c.X+c.W)*float64(w) + 0.5)
	y1 := int((c.Y+c.H)*float64(h) + 0.5)
	r := image.Rect(x0, y0, x1, y1).Intersect(image.Rect(0, 0, w, h))
	if r.Dx() < 2 || r.Dy() < 2 {
		return image.Rect(0, 0, w, h)
	}
	// ffmpeg's crop wants even numbers for most pixel formats.
	r.Max.X = r.Min.X + (r.Dx() &^ 1)
	r.Max.Y = r.Min.Y + (r.Dy() &^ 1)
	return r
}

// -noopen prints the URL instead of launching a browser, for driving the API
// from a script or a second machine.
var (
	noOpen = flag.Bool("noopen", false, "do not open a browser, just print the URL")
	port   = flag.Int("port", 0, "listen on this port instead of a free one")
)

func main() {
	flag.Parse()
	for _, t := range []string{"ffmpeg", "ffprobe"} {
		if _, err := exec.LookPath(t); err != nil {
			fmt.Printf("ERROR: %s not found on PATH.\n", t)
			fmt.Println("Install it (winget install Gyan.FFmpeg), reopen your terminal, try again.")
			fmt.Println("\nPress Enter to close.")
			fmt.Scanln()
			return
		}
	}

	mux := http.NewServeMux()
	mux.HandleFunc("/", serveUI)
	mux.HandleFunc("/api/open", apiOpen)
	mux.HandleFunc("/api/proxy", apiProxy)
	mux.HandleFunc("/api/extract", apiExtract)
	mux.HandleFunc("/api/render", apiRender)
	mux.HandleFunc("/api/frames", apiFrames)
	mux.HandleFunc("/api/frame", apiFrame)
	mux.HandleFunc("/api/export", apiExport)

	ln, err := net.Listen("tcp", fmt.Sprintf("127.0.0.1:%d", *port))
	if err != nil {
		log.Fatal(err)
	}
	url := fmt.Sprintf("http://127.0.0.1:%d", ln.Addr().(*net.TCPAddr).Port)
	fmt.Println("animtool running at", url)
	fmt.Println("Close this window to quit.")
	if !*noOpen {
		openBrowser(url)
	}
	log.Fatal(http.Serve(ln, mux))
}

func openBrowser(url string) {
	var c *exec.Cmd
	switch runtime.GOOS {
	case "windows":
		c = exec.Command("rundll32", "url.dll,FileProtocolHandler", url)
	case "darwin":
		c = exec.Command("open", url)
	default:
		c = exec.Command("xdg-open", url)
	}
	_ = c.Start()
}

func serveUI(w http.ResponseWriter, r *http.Request) {
	b, err := uiFS.ReadFile("ui.html")
	if err != nil {
		http.Error(w, err.Error(), 500)
		return
	}
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.Write(b)
}

func writeJSON(w http.ResponseWriter, v any) {
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(v)
}

func fail(w http.ResponseWriter, msg string) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(400)
	json.NewEncoder(w).Encode(map[string]string{"error": msg})
}

func clampF(v, lo, hi float64) float64 {
	return math.Min(math.Max(v, lo), hi)
}

func clampI(v, lo, hi int) int {
	if v < lo {
		return lo
	}
	if v > hi {
		return hi
	}
	return v
}

// ------------------------------------------------------- step 1: open

// apiOpen probes the video and renders the whole thing to small JPEGs on the
// 25 fps timeline. Those proxy frames are what the scrubber and the crop
// picker show, so picking a range afterwards is instant - the decode cost is
// paid once, here.
func apiOpen(w http.ResponseWriter, r *http.Request) {
	var req struct {
		Path string `json:"path"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		fail(w, "bad request")
		return
	}
	path := strings.TrimSpace(req.Path)
	if len(path) >= 2 && (path[0] == '"' || path[0] == '\'') {
		path = path[1 : len(path)-1] // tolerate pasted quoted paths
	}
	if _, err := os.Stat(path); err != nil {
		fail(w, "cannot open file: "+path)
		return
	}

	sw, sh, dur := probe(path)
	if sw == 0 {
		fail(w, "no video stream found in that file")
		return
	}
	if dur > 0 && dur*scrubFPS > maxProxyFrames {
		fail(w, fmt.Sprintf("clip is %.0f s; trim it below %d s first",
			dur, maxProxyFrames/scrubFPS))
		return
	}

	dir, err := os.MkdirTemp("", "animproxy")
	if err != nil {
		fail(w, err.Error())
		return
	}
	// 426 wide is enough to aim a crop box at, and small enough that a couple
	// of thousand of them stay cheap to write and to scrub through.
	scale := "scale=426:-2"
	if sw <= 426 {
		scale = fmt.Sprintf("scale=%d:-2", sw&^1)
	}
	cmd := exec.Command("ffmpeg", "-v", "error", "-y", "-i", path,
		"-vf", fmt.Sprintf("fps=%d,%s", scrubFPS, scale),
		"-fps_mode", "passthrough", "-q:v", "6",
		filepath.Join(dir, "p%05d.jpg"))
	if out, err := cmd.CombinedOutput(); err != nil {
		os.RemoveAll(dir)
		fail(w, "ffmpeg: "+string(out))
		return
	}
	files, _ := filepath.Glob(filepath.Join(dir, "p*.jpg"))
	if len(files) == 0 {
		os.RemoveAll(dir)
		fail(w, "no frames decoded from that file")
		return
	}

	sess.mu.Lock()
	oldProxy, oldDir := sess.proxyDir, sess.dir
	sess.video, sess.srcW, sess.srcH = path, sw, sh
	sess.proxyDir, sess.proxyN = dir, len(files)
	sess.dir, sess.gray, sess.bits, sess.prev, sess.keep = "", nil, nil, nil, nil
	sess.gen++
	generation := sess.gen
	sess.mu.Unlock()
	if oldProxy != "" {
		os.RemoveAll(oldProxy)
	}
	if oldDir != "" {
		os.RemoveAll(oldDir)
	}

	writeJSON(w, map[string]any{
		"srcW": sw, "srcH": sh,
		"duration": float64(len(files)) / scrubFPS,
		"frames":   len(files),
		"fps":      scrubFPS,
		"firstLit": firstLitFrame(dir, len(files)),
		// The scrub frames are cached hard so dragging the scrubber never
		// stalls, but /api/proxy?i=N is the same URL for every video opened.
		// The client appends this so a new video is not served the last one's
		// frames out of the browser cache.
		"gen": generation,
	})
}

// Clips routinely open on black or fade in from it, and a black frame is no
// use for aiming the crop box or judging the tone controls. These two decide
// how far in the tool will look for a frame with something on it.
const (
	litScanFrames = 125 // five seconds; past that it is not a lead-in
	litThreshold  = 24  // mean luminance out of 255
)

// firstLitFrame returns the first proxy frame that is not essentially black,
// or 0 if the opening seconds are all dark - in which case there is nothing
// better to show and frame 0 is as good as any.
func firstLitFrame(dir string, n int) int {
	limit := n
	if limit > litScanFrames {
		limit = litScanFrames
	}
	for i := 0; i < limit; i++ {
		f, err := os.Open(filepath.Join(dir, fmt.Sprintf("p%05d.jpg", i+1)))
		if err != nil {
			continue
		}
		img, err := jpeg.Decode(f)
		f.Close()
		if err != nil {
			continue
		}
		if meanLuma(img) >= litThreshold {
			return i
		}
	}
	return 0
}

// meanLuma samples every fourth pixel in each direction. At proxy size that is
// still thousands of samples - far more than enough to tell black from not.
func meanLuma(img image.Image) float64 {
	b := img.Bounds()
	var sum, n float64
	for y := b.Min.Y; y < b.Max.Y; y += 4 {
		for x := b.Min.X; x < b.Max.X; x += 4 {
			r, g, bl, _ := img.At(x, y).RGBA()
			sum += (0.299*float64(r) + 0.587*float64(g) + 0.114*float64(bl)) / 257
			n++
		}
	}
	if n == 0 {
		return 0
	}
	return sum / n
}

func probe(path string) (int, int, float64) {
	out, err := exec.Command("ffprobe", "-v", "error", "-select_streams", "v:0",
		"-show_entries", "stream=width,height:format=duration",
		"-of", "default=nw=1:nk=1", path).Output()
	if err != nil {
		return 0, 0, 0
	}
	fields := strings.Fields(string(out))
	var w, h int
	var dur float64
	if len(fields) > 0 {
		w, _ = strconv.Atoi(fields[0])
	}
	if len(fields) > 1 {
		h, _ = strconv.Atoi(fields[1])
	}
	if len(fields) > 2 {
		dur, _ = strconv.ParseFloat(fields[2], 64)
	}
	return w, h, dur
}

func apiProxy(w http.ResponseWriter, r *http.Request) {
	i, _ := strconv.Atoi(r.URL.Query().Get("i"))
	sess.mu.Lock()
	dir, n := sess.proxyDir, sess.proxyN
	sess.mu.Unlock()
	if dir == "" || i < 0 || i >= n {
		http.Error(w, "no such frame", 404)
		return
	}
	b, err := os.ReadFile(filepath.Join(dir, fmt.Sprintf("p%05d.jpg", i+1)))
	if err != nil {
		http.Error(w, "read error", 500)
		return
	}
	w.Header().Set("Content-Type", "image/jpeg")
	w.Header().Set("Cache-Control", "max-age=86400")
	w.Write(b)
}

// --------------------------------------------------- step 2: extract

// apiExtract crops at native resolution first, then scales to panel size.
// That order is what preserves detail - scaling first would throw away the
// very pixels the crop was meant to keep.
//
// The output length comes from the duration the user typed: at 25 fps,
// `count` frames play for count/25 seconds. If that asks for more frames than
// the selected range holds, source frames get reused, which holds the picture
// rather than inventing motion.
func apiExtract(w http.ResponseWriter, r *http.Request) {
	var req struct {
		Crop       cropFrac `json:"crop"`
		W          int      `json:"w"`
		H          int      `json:"h"`
		Start      int      `json:"start"`
		End        int      `json:"end"`
		Count      int      `json:"count"`
		FPS        int      `json:"fps"` // the animation's rate, not the scrub grid
		Sharpen    bool     `json:"sharpen"`
		Brightness float64  `json:"brightness"`
		Contrast   float64  `json:"contrast"`
		Gamma      float64  `json:"gamma"`
		Fit        string   `json:"fit"` // cover | contain
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		fail(w, "bad request")
		return
	}
	if req.W < 8 || req.H < 8 || req.W > 512 || req.H > 512 {
		fail(w, "output size must be between 8 and 512 px")
		return
	}
	if req.H%8 != 0 {
		fail(w, "height must be a multiple of 8 - one byte is eight stacked pixels")
		return
	}
	if req.Contrast == 0 {
		req.Contrast = 1 // absent field means "no change", not "flat grey"
	}
	if req.Gamma == 0 {
		req.Gamma = 1
	}
	if req.FPS == 0 {
		req.FPS = scrubFPS // absent field means "the usual rate"
	}
	if req.FPS < minAnimFPS || req.FPS > maxAnimFPS {
		fail(w, fmt.Sprintf("frame rate must be between %d and %d fps",
			minAnimFPS, maxAnimFPS))
		return
	}

	sess.mu.Lock()
	video, sw, sh, proxyN := sess.video, sess.srcW, sess.srcH, sess.proxyN
	oldDir := sess.dir
	sess.mu.Unlock()
	if video == "" {
		fail(w, "open a video first")
		return
	}

	start := clampI(req.Start, 0, proxyN-1)
	end := clampI(req.End, start, proxyN-1)
	span := end - start + 1
	count := clampI(req.Count, 1, 2000)

	dir, err := os.MkdirTemp("", "animframes")
	if err != nil {
		fail(w, err.Error())
		return
	}

	rc := req.Crop.rectIn(sw, sh)
	vf := fmt.Sprintf("fps=%d,crop=%d:%d:%d:%d,",
		scrubFPS, rc.Dx(), rc.Dy(), rc.Min.X, rc.Min.Y)
	if req.Fit == "contain" {
		// Letterbox: the whole crop is kept and the gaps go black. For when
		// the subject must not lose its edges.
		vf += fmt.Sprintf("scale=%d:%d:force_original_aspect_ratio=decrease:flags=lanczos,"+
			"pad=%d:%d:(ow-iw)/2:(oh-ih)/2:black,setsar=1", req.W, req.H, req.W, req.H)
	} else {
		vf += fmt.Sprintf("scale=%d:%d:force_original_aspect_ratio=increase:flags=lanczos,"+
			"crop=%d:%d,setsar=1", req.W, req.H, req.W, req.H)
	}
	if req.Sharpen {
		// A light pass restores edge definition lost to a heavy downscale.
		vf += ",unsharp=3:3:0.8"
	}
	if req.Brightness != 0 || req.Contrast != 1 || req.Gamma != 1 {
		// Done here, before the frames are cached, because these three decide
		// what counts as mid grey - and the threshold that follows is only as
		// good as the tones it is handed.
		vf += fmt.Sprintf(",eq=brightness=%.4f:contrast=%.4f:gamma=%.4f",
			clampF(req.Brightness, -1, 1),
			clampF(req.Contrast, 0, 3),
			clampF(req.Gamma, 0.1, 10))
	}
	vf += ",format=gray"

	// Input-side -ss is accurate in current ffmpeg (it seeks to the keyframe
	// then decodes forward), and unlike output-side seeking it does not decode
	// the whole file up to the start point.
	cmd := exec.Command("ffmpeg", "-v", "error", "-y",
		"-ss", fmt.Sprintf("%.4f", float64(start)/scrubFPS),
		"-i", video,
		"-t", fmt.Sprintf("%.4f", float64(span)/scrubFPS),
		"-vf", vf, "-fps_mode", "passthrough",
		filepath.Join(dir, "f%05d.png"))
	if out, err := cmd.CombinedOutput(); err != nil {
		os.RemoveAll(dir)
		fail(w, "ffmpeg: "+string(out))
		return
	}
	files, _ := filepath.Glob(filepath.Join(dir, "f*.png"))
	if len(files) == 0 {
		os.RemoveAll(dir)
		fail(w, "no frames in that range")
		return
	}

	// Resample the extracted range onto exactly `count` output frames. Nearest
	// neighbour, not blending: a blended in-between frame would be a ghost of
	// two poses, and at one bit per pixel a ghost is just noise.
	gray := make([]*image.Gray, count)
	for k := 0; k < count; k++ {
		src := 0
		if count > 1 {
			src = int(float64(k)*float64(len(files)-1)/float64(count-1) + 0.5)
		}
		g, err := loadGray(filepath.Join(dir, fmt.Sprintf("f%05d.png", src+1)))
		if err != nil {
			os.RemoveAll(dir)
			fail(w, err.Error())
			return
		}
		gray[k] = g
	}

	sess.mu.Lock()
	sess.dir, sess.gray = dir, gray
	sess.outW, sess.outH = req.W, req.H
	sess.fps = req.FPS
	sess.keep = make([]int, count)
	for i := range sess.keep {
		sess.keep[i] = i
	}
	sess.bits, sess.prev = nil, nil
	sess.mu.Unlock()
	if oldDir != "" {
		os.RemoveAll(oldDir)
	}

	writeJSON(w, map[string]any{
		"frames": count, "source": len(files), "w": req.W, "h": req.H,
		"fps":  req.FPS,
		"crop": fmt.Sprintf("%dx%d", rc.Dx(), rc.Dy()),
		// True when the duration asks for more frames than the range holds, so
		// some frames are repeats. Not an error, but worth saying out loud.
		"stretched": count > len(files),
	})
}

func loadGray(path string) (*image.Gray, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	src, err := png.Decode(f)
	if err != nil {
		return nil, err
	}
	b := src.Bounds()
	g := image.NewGray(b)
	draw.Draw(g, b, src, b.Min, draw.Src)
	return g, nil
}

// --------------------------------------------------- step 3: render

// apiRender turns the cached greyscale frames into panel bytes. It touches no
// files and runs no subprocess, so the threshold and dither controls can be
// dragged and the animation re-renders between two frames of playback.
func apiRender(w http.ResponseWriter, r *http.Request) {
	var req struct {
		Mode      string `json:"mode"` // threshold | fs | bayer
		Threshold int    `json:"threshold"`
		Invert    bool   `json:"invert"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		fail(w, "bad request")
		return
	}
	sess.mu.Lock()
	gray, keep, ow, oh := sess.gray, sess.keep, sess.outW, sess.outH
	sess.mu.Unlock()
	if len(keep) == 0 {
		fail(w, "extract frames first")
		return
	}
	th := clampI(req.Threshold, 1, 254)

	bits := make([][]byte, len(keep))
	prev := make([][]byte, len(keep))
	var lit int64
	for i, gi := range keep {
		g := gray[gi]
		on := toMono(g, req.Mode, th, req.Invert)
		bits[i] = packPages(on, ow, oh)
		prev[i] = previewPNG(on, ow, oh)
		for _, v := range on {
			if v {
				lit++
			}
		}
	}

	sess.mu.Lock()
	sess.bits, sess.prev = bits, prev
	sess.gen++
	gen := sess.gen
	sess.mu.Unlock()

	writeJSON(w, map[string]any{
		"gen": gen, "frames": len(bits), "bytesPerFrame": len(bits[0]),
		"total":     len(bits) * len(bits[0]),
		"extracted": len(gray),
		"dropped":   len(gray) - len(keep),
		// Share of pixels lit, averaged over the animation. Below about 5% the
		// picture has fallen apart into specks; above about 60% it is a white
		// slab. Either way the threshold is in the wrong place, and this says
		// so before the export does.
		"coverage": float64(lit) / float64(len(keep)*ow*oh),
	})
}

// bayer8 is the classic ordered-dither matrix, values 0..63. Ordered dithering
// keeps its texture still between frames, which on a small animation reads as
// a screen door rather than the crawling fizz error diffusion produces.
var bayer8 = [8][8]int{
	{0, 32, 8, 40, 2, 34, 10, 42},
	{48, 16, 56, 24, 50, 18, 58, 26},
	{12, 44, 4, 36, 14, 46, 6, 38},
	{60, 28, 52, 20, 62, 30, 54, 22},
	{3, 35, 11, 43, 1, 33, 9, 41},
	{51, 19, 59, 27, 49, 17, 57, 25},
	{15, 47, 7, 39, 13, 45, 5, 37},
	{63, 31, 55, 23, 61, 29, 53, 21},
}

// toMono returns one bool per pixel in row-major order: true means lit.
func toMono(g *image.Gray, mode string, th int, invert bool) []bool {
	b := g.Bounds()
	w, h := b.Dx(), b.Dy()
	out := make([]bool, w*h)

	switch mode {
	case "fs":
		// Floyd-Steinberg on a float copy, so the error pushed into the
		// neighbours is not clipped by an 8-bit store on the way.
		buf := make([]float64, w*h)
		for y := 0; y < h; y++ {
			for x := 0; x < w; x++ {
				buf[y*w+x] = float64(g.GrayAt(b.Min.X+x, b.Min.Y+y).Y)
			}
		}
		push := func(x, y int, e float64) {
			if x >= 0 && x < w && y < h {
				buf[y*w+x] += e
			}
		}
		for y := 0; y < h; y++ {
			for x := 0; x < w; x++ {
				old := buf[y*w+x]
				var quant float64
				if old >= float64(th) {
					quant, out[y*w+x] = 255, true
				}
				e := old - quant
				push(x+1, y, e*7.0/16)
				push(x-1, y+1, e*3.0/16)
				push(x, y+1, e*5.0/16)
				push(x+1, y+1, e*1.0/16)
			}
		}
	case "bayer":
		for y := 0; y < h; y++ {
			for x := 0; x < w; x++ {
				v := int(g.GrayAt(b.Min.X+x, b.Min.Y+y).Y)
				// Centre the matrix on the threshold so the slider still means
				// "how bright before it lights up".
				bias := (bayer8[y&7][x&7]-32)*255/64 + th
				out[y*w+x] = v >= bias
			}
		}
	default: // hard threshold
		for y := 0; y < h; y++ {
			for x := 0; x < w; x++ {
				out[y*w+x] = int(g.GrayAt(b.Min.X+x, b.Min.Y+y).Y) >= th
			}
		}
	}

	if invert {
		for i := range out {
			out[i] = !out[i]
		}
	}
	return out
}

// packPages lays the frame out the way the SH1106/SSD1306 framebuffer does:
// data[page*w + x], bit (y&7), bit 0 the topmost row of the page. A frame in
// this layout is a memcpy into fb[][], not a per-pixel loop.
func packPages(on []bool, w, h int) []byte {
	out := make([]byte, (h/8)*w)
	for y := 0; y < h; y++ {
		for x := 0; x < w; x++ {
			if on[y*w+x] {
				out[(y/8)*w+x] |= 1 << uint(y&7)
			}
		}
	}
	return out
}

// packRows lays the frame out MSB-first along each row: data[y*stride + x/8],
// bit (7 - x&7). This is the XBM-style layout most desktop tools and generic
// framebuffer blitters expect.
func packRows(on []bool, w, h int) []byte {
	stride := (w + 7) / 8
	out := make([]byte, stride*h)
	for y := 0; y < h; y++ {
		for x := 0; x < w; x++ {
			if on[y*w+x] {
				out[y*stride+x/8] |= 0x80 >> uint(x&7)
			}
		}
	}
	return out
}

// unpackPages is the inverse of packPages. Export re-derives every frame from
// the packed bytes rather than from the greyscale, so what ships is provably
// the same picture the preview showed.
func unpackPages(b []byte, w, h int) []bool {
	on := make([]bool, w*h)
	for y := 0; y < h; y++ {
		for x := 0; x < w; x++ {
			on[y*w+x] = b[(y/8)*w+x]&(1<<uint(y&7)) != 0
		}
	}
	return on
}

var monoPalette = color.Palette{
	color.RGBA{0, 0, 0, 255},
	color.RGBA{190, 235, 255, 255}, // the blue-white a lit OLED pixel actually is
}

func previewPNG(on []bool, w, h int) []byte {
	img := image.NewPaletted(image.Rect(0, 0, w, h), monoPalette)
	for i, v := range on {
		if v {
			img.Pix[i] = 1
		}
	}
	var buf bytes.Buffer
	png.Encode(&buf, img)
	return buf.Bytes()
}

// apiFrames edits which extracted frames the animation is made of. Resampling
// picks frames by time, so it has no way of knowing that one of them caught a
// cut, a blink or a camera flash - which at one bit per pixel is a hole in the
// loop. This takes them out.
//
// Nothing is destroyed: the greyscale stays, only the keep list shrinks, so
// Reset puts everything back without touching ffmpeg. The caller re-renders
// afterwards, because only it knows the current threshold and dither.
func apiFrames(w http.ResponseWriter, r *http.Request) {
	var req struct {
		Drop  []int `json:"drop"`  // positions in the current animation
		Reset bool  `json:"reset"` // put every extracted frame back
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		fail(w, "bad request")
		return
	}

	sess.mu.Lock()
	defer sess.mu.Unlock()
	if len(sess.gray) == 0 {
		fail(w, "extract frames first")
		return
	}

	if req.Reset {
		sess.keep = make([]int, len(sess.gray))
		for i := range sess.keep {
			sess.keep[i] = i
		}
		writeJSON(w, map[string]any{
			"frames": len(sess.keep), "extracted": len(sess.gray), "dropped": 0})
		return
	}

	drop := map[int]bool{}
	for _, i := range req.Drop {
		if i < 0 || i >= len(sess.keep) {
			fail(w, fmt.Sprintf("frame %d is not in the animation", i))
			return
		}
		drop[i] = true
	}
	if len(drop) == 0 {
		fail(w, "no frames given to remove")
		return
	}
	if len(drop) >= len(sess.keep) {
		// An empty animation has nothing to preview and nothing to export, and
		// the way back is not obvious once the screen is blank.
		fail(w, "that would remove every frame - use Restore all instead")
		return
	}

	kept := make([]int, 0, len(sess.keep)-len(drop))
	for i, gi := range sess.keep {
		if !drop[i] {
			kept = append(kept, gi)
		}
	}
	sess.keep = kept

	writeJSON(w, map[string]any{
		"frames": len(kept), "extracted": len(sess.gray),
		"dropped": len(sess.gray) - len(kept)})
}

func apiFrame(w http.ResponseWriter, r *http.Request) {
	i, _ := strconv.Atoi(r.URL.Query().Get("i"))
	sess.mu.Lock()
	prev := sess.prev
	sess.mu.Unlock()
	if i < 0 || i >= len(prev) {
		http.Error(w, "no such frame", 404)
		return
	}
	w.Header().Set("Content-Type", "image/png")
	w.Header().Set("Cache-Control", "max-age=86400")
	w.Write(prev[i])
}

// ------------------------------------------------------ step 4: export

func apiExport(w http.ResponseWriter, r *http.Request) {
	var req struct {
		Dir        string `json:"dir"`
		Name       string `json:"name"`
		Layout     string `json:"layout"`     // pages | rows
		HeaderOnly bool   `json:"headerOnly"` // one .h, no .c
		InFlash    bool   `json:"inFlash"`    // pico __in_flash section
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		fail(w, "bad request")
		return
	}
	sess.mu.Lock()
	bits, ow, oh, fps := sess.bits, sess.outW, sess.outH, sess.fps
	video := sess.video
	sess.mu.Unlock()
	if len(bits) == 0 {
		fail(w, "render the frames first")
		return
	}

	name := sanitiseIdent(req.Name)
	if name == "" {
		fail(w, "give the animation a name (letters, digits, underscore)")
		return
	}
	dir := strings.TrimSpace(req.Dir)
	if dir == "" {
		dir = "."
	}
	if err := os.MkdirAll(dir, 0o755); err != nil {
		fail(w, err.Error())
		return
	}

	frames := make([][]byte, len(bits))
	for i, b := range bits {
		if req.Layout == "rows" {
			frames[i] = packRows(unpackPages(b, ow, oh), ow, oh)
		} else {
			frames[i] = b
		}
	}

	hSrc, cSrc := renderSource(name, filepath.Base(video), ow, oh, fps, frames,
		req.Layout, req.HeaderOnly, req.InFlash)

	hPath := filepath.Join(dir, name+".h")
	if err := os.WriteFile(hPath, hSrc, 0o644); err != nil {
		fail(w, err.Error())
		return
	}
	written := []string{hPath}
	if cSrc != nil {
		cPath := filepath.Join(dir, name+".c")
		if err := os.WriteFile(cPath, cSrc, 0o644); err != nil {
			fail(w, err.Error())
			return
		}
		written = append(written, cPath)
	}
	for i := range written {
		if abs, err := filepath.Abs(written[i]); err == nil {
			written[i] = abs
		}
	}

	writeJSON(w, map[string]any{
		"files":    written,
		"frames":   len(frames),
		"bytes":    len(frames) * len(frames[0]),
		"perFrame": len(frames[0]),
		"seconds":  float64(len(frames)) / float64(fps),
		"fps":      fps,
	})
}

func sanitiseIdent(s string) string {
	var b strings.Builder
	for i, r := range strings.TrimSpace(s) {
		switch {
		case r >= 'a' && r <= 'z', r >= 'A' && r <= 'Z', r == '_':
			b.WriteRune(r)
		case r >= '0' && r <= '9':
			if i == 0 {
				b.WriteByte('_') // an identifier may not start with a digit
			}
			b.WriteRune(r)
		case r == ' ' || r == '-' || r == '.':
			b.WriteByte('_')
		}
	}
	return b.String()
}

// renderSource writes the .h (and .c, unless header-only) for the animation.
// Returns nil for the .c when everything went into the header.
func renderSource(name, src string, w, h, fps int, frames [][]byte,
	layout string, headerOnly, inFlash bool) ([]byte, []byte) {

	up := strings.ToUpper(name)
	perFrame := len(frames[0])
	guard := "NOVA_" + up + "_ANIM_H"

	var layoutNote, strideNote string
	if layout == "rows" {
		layoutNote = fmt.Sprintf(
			"// Layout: row-packed, MSB first. data[y * %s_STRIDE + (x >> 3)],\n"+
				"// bit (7 - (x & 7)) - the XBM order, for a generic blitter.", up)
		strideNote = fmt.Sprintf("#define %s_STRIDE   %d   // bytes per pixel row\n",
			up, (w+7)/8)
	} else {
		layoutNote = fmt.Sprintf(
			"// Layout: page-packed. data[page * %s_W + x], bit (y & 7), bit 0 the\n"+
				"// topmost row of the page - the same order as sh1106.c's fb[page][x],\n"+
				"// so one frame is a memcpy into the framebuffer, not a pixel loop.", up)
		strideNote = fmt.Sprintf("#define %s_PAGES    %d   // rows of 8 stacked pixels\n",
			up, h/8)
	}

	var hb strings.Builder
	fmt.Fprintf(&hb, "// Generated by oled_vis/animtool from %q - do not edit.\n//\n", src)
	fmt.Fprintf(&hb, "// %d frames, %dx%d, %d fps: %.2f s per loop, %d bytes total.\n",
		len(frames), w, h, fps, float64(len(frames))/float64(fps),
		len(frames)*perFrame)
	fmt.Fprintf(&hb, "%s\n", layoutNote)
	fmt.Fprintf(&hb, "#ifndef %s\n#define %s\n\n#include <stdint.h>\n\n", guard, guard)
	fmt.Fprintf(&hb, "#define %s_W        %d\n", up, w)
	fmt.Fprintf(&hb, "#define %s_H        %d\n", up, h)
	hb.WriteString(strideNote)
	fmt.Fprintf(&hb, "#define %s_BYTES    %d   // bytes in one frame\n", up, perFrame)
	fmt.Fprintf(&hb, "#define %s_FRAMES   %d\n", up, len(frames))
	fmt.Fprintf(&hb, "#define %s_FPS      %d\n", up, fps)
	// Microseconds as well as milliseconds: at rates that do not divide 1000
	// evenly - 30, 15, 12 - the rounded millisecond figure drifts by about a
	// frame every few seconds, and a loop timed off the microsecond value does
	// not. 25 fps divides evenly and gets no note.
	note := "// hold each frame this long"
	if 1000%fps != 0 {
		note = fmt.Sprintf("// rounded from %.2f - time long loops off _FRAME_US",
			1000/float64(fps))
	}
	fmt.Fprintf(&hb, "#define %s_FRAME_MS %d   %s\n", up, (1000+fps/2)/fps, note)
	fmt.Fprintf(&hb, "#define %s_FRAME_US %d\n\n", up, 1000000/fps)

	if headerOnly {
		// static, because a header carrying its own data is included once by
		// design; two translation units including it would otherwise each get
		// their own copy in flash.
		fmt.Fprintf(&hb, "static const uint8_t %s_frames[%s_FRAMES][%s_BYTES] = {\n",
			name, up, up)
		writeFrames(&hb, frames, w, layout)
		hb.WriteString("};\n\n")
		fmt.Fprintf(&hb, "#endif // %s\n", guard)
		return []byte(hb.String()), nil
	}

	fmt.Fprintf(&hb, "extern const uint8_t %s_frames[%s_FRAMES][%s_BYTES];\n\n",
		name, up, up)
	fmt.Fprintf(&hb, "#endif // %s\n", guard)

	var cb strings.Builder
	fmt.Fprintf(&cb, "// Generated by oled_vis/animtool from %q - do not edit.\n", src)
	if inFlash {
		cb.WriteString("//\n// __in_flash() keeps this out of SRAM: .flashdata stays in flash even\n" +
			"// in a copy_to_ram build, and is read straight through the XIP window.\n")
	}
	fmt.Fprintf(&cb, "\n#include \"%s.h\"\n", name)
	if inFlash {
		cb.WriteString("#include \"pico/platform/sections.h\"\n")
	}
	cb.WriteString("\n")
	attr := ""
	if inFlash {
		attr = fmt.Sprintf(" __in_flash(\"%s\")", truncSection(name))
	}
	fmt.Fprintf(&cb, "const uint8_t %s_frames[%s_FRAMES][%s_BYTES]%s = {\n",
		name, up, up, attr)
	writeFrames(&cb, frames, w, layout)
	cb.WriteString("};\n")

	return []byte(hb.String()), []byte(cb.String())
}

// truncSection keeps the linker section name short. Long names are legal but
// make the map file unreadable.
func truncSection(name string) string {
	if len(name) > 12 {
		return name[:12]
	}
	return name
}

// writeFrames emits the initialiser rows. Bytes are broken at the width of one
// panel row so the array reads as a picture in the source: `w` bytes for the
// page layout, the row stride for the row layout.
func writeFrames(b *strings.Builder, frames [][]byte, w int, layout string) {
	per := w
	if layout == "rows" {
		per = (w + 7) / 8
	}
	if per > 32 {
		per = 32 // keep lines from running off the far right of the editor
	}
	for i, f := range frames {
		fmt.Fprintf(b, "    { // frame %d\n", i)
		for off := 0; off < len(f); off += per {
			end := off + per
			if end > len(f) {
				end = len(f)
			}
			b.WriteString("        ")
			for _, v := range f[off:end] {
				fmt.Fprintf(b, "0x%02X,", v)
			}
			b.WriteString("\n")
		}
		if i == len(frames)-1 {
			b.WriteString("    }\n")
		} else {
			b.WriteString("    },\n")
		}
	}
}
