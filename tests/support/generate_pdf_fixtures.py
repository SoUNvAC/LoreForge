from __future__ import annotations

from io import BytesIO
from pathlib import Path

from PIL import Image, ImageDraw
from reportlab.lib.pagesizes import letter
from reportlab.lib.utils import ImageReader
from reportlab.pdfgen import canvas


ROOT = Path(__file__).resolve().parents[1] / "fixtures" / "pdf"
PAGE_WIDTH, PAGE_HEIGHT = letter


def new_canvas(path: Path, title: str, author: str = "LoreForge Tests") -> canvas.Canvas:
    pdf = canvas.Canvas(str(path), pagesize=letter, invariant=1, pageCompression=0)
    pdf.setTitle(title)
    pdf.setAuthor(author)
    pdf.setCreator("LoreForge deterministic fixture generator")
    return pdf


def draw_line(pdf: canvas.Canvas, x: float, y: float, text: str, size: float = 12) -> None:
    pdf.setFont("Helvetica", size)
    pdf.drawString(x, y, text)


def simple_pdf() -> None:
    pdf = new_canvas(ROOT / "simple.pdf", "Simple PDF Fixture", "Ada Example")
    draw_line(pdf, 72, 730, "Chapter 1", 18)
    draw_line(pdf, 72, 690, "The first page begins here.")
    draw_line(pdf, 72, 668, "It keeps a second paragraph.")
    draw_line(pdf, 286, 40, "1", 9)
    pdf.showPage()
    draw_line(pdf, 72, 730, "Chapter 2", 18)
    draw_line(pdf, 72, 690, "The second page has its own chapter.")
    draw_line(pdf, 286, 40, "2", 9)
    pdf.save()


def two_column_pdf() -> None:
    pdf = new_canvas(ROOT / "two_column.pdf", "Two Column Fixture")
    draw_line(pdf, 72, 740, "Chapter 1", 18)
    # Paint the right column first so content-stream order cannot be mistaken for reading order.
    draw_line(pdf, 330, 690, "Right column starts third.")
    draw_line(pdf, 330, 666, "Right column finishes fourth.")
    draw_line(pdf, 72, 690, "Left column starts first.")
    draw_line(pdf, 72, 666, "Left column continues second.")
    draw_line(pdf, 286, 40, "1", 9)
    pdf.save()


def ambiguous_pdf() -> None:
    pdf = new_canvas(ROOT / "ambiguous.pdf", "Ambiguous Layout Fixture")
    draw_line(pdf, 72, 740, "Chapter 1", 18)
    draw_line(pdf, 72, 690, "A left fragment.")
    draw_line(pdf, 238, 690, "A center fragment.")
    draw_line(pdf, 404, 690, "A right fragment.")
    draw_line(pdf, 72, 660, "The layout has three competing lanes.")
    pdf.save()


def scanned_pdf() -> None:
    pdf = new_canvas(ROOT / "scanned.pdf", "Scanned Fixture")
    image = Image.new("RGB", (900, 1200), "white")
    drawing = ImageDraw.Draw(image)
    drawing.rectangle((35, 35, 865, 1165), outline="black", width=4)
    drawing.text((90, 140), "SCANNED PAGE - OCR REQUIRED", fill="black")
    drawing.text((90, 220), "This sentence exists only in image pixels.", fill="black")
    encoded = BytesIO()
    image.save(encoded, format="PNG", optimize=False)
    encoded.seek(0)
    pdf.drawImage(ImageReader(encoded), 36, 36, PAGE_WIDTH - 72, PAGE_HEIGHT - 72)
    pdf.save()


def main() -> None:
    ROOT.mkdir(parents=True, exist_ok=True)
    simple_pdf()
    two_column_pdf()
    ambiguous_pdf()
    scanned_pdf()


if __name__ == "__main__":
    main()
