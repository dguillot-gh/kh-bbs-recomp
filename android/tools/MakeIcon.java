// Makes the launcher icon from the disc's own title picture (PSP_GAME/PIC1.PNG): the logo, centred on black.
// Run at build time only, so nothing made from the disc is ever committed.
//   java MakeIcon.java <PIC1.PNG or ""> <res dir>     writes <res>/mipmap-*/ic_launcher.png
import javax.imageio.ImageIO;
import java.awt.*;
import java.awt.image.BufferedImage;
import java.io.File;

public class MakeIcon {
    public static void main(String[] a) throws Exception {
        File in = new File(a[0]);
        BufferedImage pic = in.isFile() ? ImageIO.read(in) : null;
        String[][] sizes = {{"mdpi", "48"}, {"hdpi", "72"}, {"xhdpi", "96"}, {"xxhdpi", "144"}, {"xxxhdpi", "192"}};
        for (String[] s : sizes) {
            int n = Integer.parseInt(s[1]);
            BufferedImage out = new BufferedImage(n, n, BufferedImage.TYPE_INT_RGB);
            Graphics2D g = out.createGraphics();
            g.setColor(Color.BLACK); g.fillRect(0, 0, n, n);
            g.setRenderingHint(RenderingHints.KEY_INTERPOLATION, RenderingHints.VALUE_INTERPOLATION_BICUBIC);
            g.setRenderingHint(RenderingHints.KEY_RENDERING, RenderingHints.VALUE_RENDER_QUALITY);
            if (pic != null) {
                // The logo sits top-centre of the 480x272 picture; scale the box to the icon width.
                double sx = pic.getWidth() / 480.0, sy = pic.getHeight() / 272.0;
                int x0 = (int) (212 * sx), y0 = (int) (0 * sy), x1 = (int) (388 * sx), y1 = (int) (116 * sy);
                int w = n, h = (int) Math.round(n * (double) (y1 - y0) / (x1 - x0));
                int top = (n - h) / 2;
                g.drawImage(pic, 0, top, w, top + h, x0, y0, x1, y1, null);
            }
            g.dispose();
            File d = new File(a[1], "mipmap-" + s[0]); d.mkdirs();
            ImageIO.write(out, "png", new File(d, "ic_launcher.png"));
        }
    }
}
