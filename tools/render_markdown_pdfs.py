#!/usr/bin/env python3

from __future__ import annotations

import argparse
import html
import math
from pathlib import Path
from typing import Iterable

import markdown as md
from bs4 import BeautifulSoup, NavigableString, Tag
from reportlab.lib import colors
from reportlab.lib.colors import Color, HexColor
from reportlab.lib.enums import TA_CENTER, TA_LEFT
from reportlab.lib.fonts import addMapping
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import ParagraphStyle, StyleSheet1, getSampleStyleSheet
from reportlab.lib.units import mm
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.platypus import (
    HRFlowable,
    KeepTogether,
    ListFlowable,
    ListItem,
    LongTable,
    PageBreak,
    Paragraph,
    Preformatted,
    SimpleDocTemplate,
    Spacer,
    Table,
    TableStyle,
)


PAGE_SIZE = A4
LEFT_MARGIN = 19 * mm
RIGHT_MARGIN = 19 * mm
TOP_MARGIN = 20 * mm
BOTTOM_MARGIN = 18 * mm

ACCENT = HexColor("#0F766E")
ACCENT_SOFT = HexColor("#D9F3EF")
TEXT = HexColor("#17212B")
TEXT_SOFT = HexColor("#52606D")
RULE = HexColor("#D7DEE7")
SURFACE = HexColor("#F6F8FB")
SURFACE_ALT = HexColor("#EEF3F7")
CODE_BG = HexColor("#F2F4F7")
CODE_BORDER = HexColor("#D8DEE6")

BODY_FONT_RAW = "AppleGothic"
MONO_FONT = "Courier"


def register_fonts() -> None:
    font_path = Path("/System/Library/Fonts/Supplemental/AppleGothic.ttf")
    pdfmetrics.registerFont(TTFont(BODY_FONT_RAW, str(font_path)))
    addMapping(BODY_FONT_RAW, 0, 0, BODY_FONT_RAW)
    addMapping(BODY_FONT_RAW, 1, 0, BODY_FONT_RAW)
    addMapping(BODY_FONT_RAW, 0, 1, BODY_FONT_RAW)
    addMapping(BODY_FONT_RAW, 1, 1, BODY_FONT_RAW)


def make_styles() -> StyleSheet1:
    styles = getSampleStyleSheet()

    styles.add(
        ParagraphStyle(
            "BodyKorean",
            parent=styles["BodyText"],
            fontName=BODY_FONT_RAW,
            fontSize=10.4,
            leading=16,
            textColor=TEXT,
            spaceAfter=4,
            wordWrap="CJK",
        )
    )
    styles.add(
        ParagraphStyle(
            "TitleKorean",
            parent=styles["Title"],
            fontName=BODY_FONT_RAW,
            fontSize=23,
            leading=30,
            textColor=TEXT,
            alignment=TA_LEFT,
            spaceAfter=8,
            wordWrap="CJK",
        )
    )
    styles.add(
        ParagraphStyle(
            "MetaKorean",
            parent=styles["BodyText"],
            fontName=BODY_FONT_RAW,
            fontSize=8.4,
            leading=12,
            textColor=TEXT_SOFT,
            alignment=TA_LEFT,
            wordWrap="CJK",
        )
    )
    styles.add(
        ParagraphStyle(
            "Heading2Korean",
            parent=styles["Heading2"],
            fontName=BODY_FONT_RAW,
            fontSize=15.8,
            leading=22,
            textColor=ACCENT,
            spaceBefore=16,
            spaceAfter=7,
            wordWrap="CJK",
        )
    )
    styles.add(
        ParagraphStyle(
            "Heading3Korean",
            parent=styles["Heading3"],
            fontName=BODY_FONT_RAW,
            fontSize=12.5,
            leading=17,
            textColor=TEXT,
            spaceBefore=10,
            spaceAfter=4,
            wordWrap="CJK",
        )
    )
    styles.add(
        ParagraphStyle(
            "Heading4Korean",
            parent=styles["Heading4"],
            fontName=BODY_FONT_RAW,
            fontSize=11.2,
            leading=15,
            textColor=TEXT,
            spaceBefore=8,
            spaceAfter=3,
            wordWrap="CJK",
        )
    )
    styles.add(
        ParagraphStyle(
            "BulletKorean",
            parent=styles["BodyText"],
            fontName=BODY_FONT_RAW,
            fontSize=10.2,
            leading=15.4,
            textColor=TEXT,
            leftIndent=0,
            firstLineIndent=0,
            wordWrap="CJK",
        )
    )
    styles.add(
        ParagraphStyle(
            "TableHeaderKorean",
            parent=styles["BodyText"],
            fontName=BODY_FONT_RAW,
            fontSize=8.0,
            leading=11,
            textColor=colors.white,
            alignment=TA_CENTER,
            wordWrap="CJK",
        )
    )
    styles.add(
        ParagraphStyle(
            "TableCellKorean",
            parent=styles["BodyText"],
            fontName=BODY_FONT_RAW,
            fontSize=7.9,
            leading=10.6,
            textColor=TEXT,
            wordWrap="CJK",
        )
    )
    styles.add(
        ParagraphStyle(
            "CodeKorean",
            parent=styles["Code"],
            fontName=MONO_FONT,
            fontSize=8.2,
            leading=10.2,
            textColor=TEXT,
            leftIndent=0,
            rightIndent=0,
        )
    )
    return styles


def escape_text(text: str) -> str:
    return html.escape(text, quote=False)


def has_non_ascii(text: str) -> bool:
    return any(ord(char) > 127 for char in text)


def inline_markup(node: Tag | NavigableString | None) -> str:
    if node is None:
        return ""
    if isinstance(node, NavigableString):
        return escape_text(str(node))
    if not isinstance(node, Tag):
        return ""

    name = node.name.lower()
    children = "".join(inline_markup(child) for child in node.children)

    if name in {"strong", "b"}:
        return f"<b>{children}</b>"
    if name in {"em", "i"}:
        return f"<i>{children}</i>"
    if name == "code":
        code_text = node.get_text()
        code_font = BODY_FONT_RAW if has_non_ascii(code_text) else MONO_FONT
        return f'<font face="{code_font}" color="#0F172A">{escape_text(code_text)}</font>'
    if name == "br":
        return "<br/>"
    if name == "a":
        return f'<font color="{ACCENT}"><u>{children}</u></font>'
    return children


def block_text(element: Tag) -> str:
    return "".join(inline_markup(child) for child in element.children).strip()


def table_cell_text(cell: Tag) -> str:
    return " ".join(cell.stripped_strings)


def heading_block(text: str, level: int, styles: StyleSheet1, doc_width: float) -> Table | Paragraph:
    if level == 1:
        return Paragraph(text, styles["TitleKorean"])
    if level == 2:
        return Table(
            [["", Paragraph(text, styles["Heading2Korean"])]],
            colWidths=[7, doc_width - 7],
            style=TableStyle(
                [
                    ("BACKGROUND", (0, 0), (0, 0), ACCENT),
                    ("VALIGN", (0, 0), (-1, -1), "MIDDLE"),
                    ("LEFTPADDING", (0, 0), (-1, -1), 0),
                    ("RIGHTPADDING", (0, 0), (-1, -1), 0),
                    ("TOPPADDING", (0, 0), (-1, -1), 0),
                    ("BOTTOMPADDING", (0, 0), (-1, -1), 0),
                ]
            ),
        )
    if level == 3:
        return Paragraph(text, styles["Heading3Korean"])
    return Paragraph(text, styles["Heading4Korean"])


def list_flowable(list_tag: Tag, styles: StyleSheet1) -> ListFlowable:
    ordered = list_tag.name.lower() == "ol"
    items = []

    for li in list_tag.find_all("li", recursive=False):
        main_parts = []
        nested_lists = []
        for child in li.children:
            if isinstance(child, Tag) and child.name.lower() in {"ul", "ol"}:
                nested_lists.append(child)
            else:
                main_parts.append(inline_markup(child))
        main_text = "".join(main_parts).strip() or escape_text(li.get_text(" ", strip=True))

        flows: list = [Paragraph(main_text, styles["BulletKorean"])]
        for nested in nested_lists:
            flows.append(list_flowable(nested, styles))

        items.append(ListItem(flows))

    kwargs = dict(
        leftIndent=16,
        bulletFontName=BODY_FONT_RAW,
        bulletFontSize=9.6,
        bulletColor=ACCENT if ordered else TEXT,
        spaceBefore=2,
        spaceAfter=6,
    )
    if ordered:
        kwargs["bulletType"] = "1"
        kwargs["start"] = "1"
    else:
        kwargs["bulletType"] = "bullet"
        kwargs["bulletChar"] = "•"

    return ListFlowable(items, **kwargs)


def code_block(pre_tag: Tag, styles: StyleSheet1, doc_width: float) -> Table:
    text = pre_tag.get_text("\n").strip("\n")
    code_style = styles["CodeKorean"].clone("CodeUnicode" if has_non_ascii(text) else "CodeAscii")
    code_style.fontName = BODY_FONT_RAW if has_non_ascii(text) else MONO_FONT
    code = Preformatted(text, code_style)
    return Table(
        [[code]],
        colWidths=[doc_width],
        style=TableStyle(
            [
                ("BACKGROUND", (0, 0), (-1, -1), CODE_BG),
                ("BOX", (0, 0), (-1, -1), 0.8, CODE_BORDER),
                ("LEFTPADDING", (0, 0), (-1, -1), 10),
                ("RIGHTPADDING", (0, 0), (-1, -1), 10),
                ("TOPPADDING", (0, 0), (-1, -1), 8),
                ("BOTTOMPADDING", (0, 0), (-1, -1), 8),
            ]
        ),
    )


def measure_columns(rows: list[list[str]], available_width: float) -> list[float]:
    col_count = max(len(row) for row in rows)
    weights = [1.0] * col_count

    for col in range(col_count):
        longest = 0
        for row in rows:
            if col < len(row):
                longest = max(longest, len(row[col]))
        weights[col] = max(1.0, math.sqrt(longest + 3))

    total = sum(weights)
    widths = [available_width * weight / total for weight in weights]
    min_width = 42
    widths = [max(min_width, width) for width in widths]
    total = sum(widths)
    if total > available_width:
        scale = available_width / total
        widths = [width * scale for width in widths]
    return widths


def table_block(table_tag: Tag, styles: StyleSheet1, doc_width: float) -> LongTable:
    rows_markup: list[list[Paragraph]] = []
    raw_rows: list[list[str]] = []
    header_rows = 0

    trs = table_tag.find_all("tr")
    for row_idx, tr in enumerate(trs):
        cells = tr.find_all(["th", "td"], recursive=False)
        if not cells:
            cells = tr.find_all(["th", "td"])
        is_header = row_idx == 0 or all(cell.name.lower() == "th" for cell in cells)
        if is_header and row_idx == header_rows:
            header_rows += 1

        row_texts = [table_cell_text(cell) for cell in cells]
        raw_rows.append(row_texts)
        para_style = styles["TableHeaderKorean"] if is_header else styles["TableCellKorean"]
        rows_markup.append([Paragraph(block_text(cell) or "&nbsp;", para_style) for cell in cells])

    col_widths = measure_columns(raw_rows, doc_width)
    table = LongTable(rows_markup, colWidths=col_widths, repeatRows=max(header_rows, 1))
    table.setStyle(
        TableStyle(
            [
                ("BACKGROUND", (0, 0), (-1, max(header_rows - 1, 0)), ACCENT),
                ("TEXTCOLOR", (0, 0), (-1, max(header_rows - 1, 0)), colors.white),
                ("BACKGROUND", (0, max(header_rows, 1)), (-1, -1), colors.white),
                ("ROWBACKGROUNDS", (0, max(header_rows, 1)), (-1, -1), [colors.white, SURFACE]),
                ("GRID", (0, 0), (-1, -1), 0.45, RULE),
                ("BOX", (0, 0), (-1, -1), 0.7, RULE),
                ("VALIGN", (0, 0), (-1, -1), "TOP"),
                ("LEFTPADDING", (0, 0), (-1, -1), 6),
                ("RIGHTPADDING", (0, 0), (-1, -1), 6),
                ("TOPPADDING", (0, 0), (-1, -1), 5),
                ("BOTTOMPADDING", (0, 0), (-1, -1), 5),
            ]
        )
    )
    return table


def html_to_story(html_text: str, styles: StyleSheet1, doc_width: float, source_name: str) -> tuple[str, list]:
    soup = BeautifulSoup(f"<div>{html_text}</div>", "html.parser")
    root = soup.div
    story = []
    title = Path(source_name).stem
    first_h1_consumed = False

    for element in root.children:
        if isinstance(element, NavigableString):
            if str(element).strip():
                story.append(Paragraph(escape_text(str(element).strip()), styles["BodyKorean"]))
            continue
        if not isinstance(element, Tag):
            continue

        name = element.name.lower()

        if name == "h1" and not first_h1_consumed:
            title = element.get_text(" ", strip=True)
            story.append(Paragraph(title, styles["TitleKorean"]))
            story.append(Paragraph("정보 전달을 최우선으로 두고, 읽는 흐름이 자연스럽도록 PDF로 재구성한 문서", styles["MetaKorean"]))
            story.append(Spacer(1, 3))
            story.append(HRFlowable(width="100%", thickness=1.2, color=ACCENT))
            story.append(Spacer(1, 8))
            first_h1_consumed = True
            continue

        if name in {"h1", "h2", "h3", "h4"}:
            heading = heading_block(block_text(element), int(name[1]), styles, doc_width)
            story.append(Spacer(1, 3 if name == "h2" else 1))
            story.append(heading)
            story.append(Spacer(1, 2))
            continue

        if name == "p":
            story.append(Paragraph(block_text(element), styles["BodyKorean"]))
            story.append(Spacer(1, 3))
            continue

        if name in {"ul", "ol"}:
            story.append(list_flowable(element, styles))
            continue

        if name == "pre":
            story.append(code_block(element, styles, doc_width))
            story.append(Spacer(1, 8))
            continue

        if name == "table":
            story.append(Spacer(1, 4))
            story.append(table_block(element, styles, doc_width))
            story.append(Spacer(1, 10))
            continue

        if name == "hr":
            story.append(Spacer(1, 6))
            story.append(HRFlowable(width="100%", thickness=0.7, color=RULE))
            story.append(Spacer(1, 6))
            continue

        if name == "blockquote":
            text = block_text(element)
            block = Table(
                [[Paragraph(text, styles["BodyKorean"])]],
                colWidths=[doc_width],
                style=TableStyle(
                    [
                        ("BACKGROUND", (0, 0), (-1, -1), SURFACE_ALT),
                        ("BOX", (0, 0), (-1, -1), 0.8, RULE),
                        ("LEFTPADDING", (0, 0), (-1, -1), 10),
                        ("RIGHTPADDING", (0, 0), (-1, -1), 10),
                        ("TOPPADDING", (0, 0), (-1, -1), 8),
                        ("BOTTOMPADDING", (0, 0), (-1, -1), 8),
                    ]
                ),
            )
            story.append(block)
            story.append(Spacer(1, 8))
            continue

        text = block_text(element)
        if text:
            story.append(Paragraph(text, styles["BodyKorean"]))
            story.append(Spacer(1, 4))

    return title, story


def draw_page_frame(canvas, doc, title: str, first_page: bool) -> None:
    page_width, page_height = doc.pagesize
    canvas.saveState()

    if first_page:
        canvas.setFillColor(ACCENT_SOFT)
        canvas.roundRect(doc.leftMargin, page_height - 34, 145, 18, 6, fill=1, stroke=0)
        canvas.setFillColor(ACCENT)
        canvas.setFont(BODY_FONT_RAW, 8.2)
        canvas.drawString(doc.leftMargin + 10, page_height - 27.5, "Styled PDF Edition")
    else:
        canvas.setStrokeColor(ACCENT)
        canvas.setLineWidth(1.1)
        canvas.line(doc.leftMargin, page_height - 20, page_width - doc.rightMargin, page_height - 20)
        canvas.setFillColor(TEXT_SOFT)
        canvas.setFont(BODY_FONT_RAW, 7.9)
        canvas.drawString(doc.leftMargin, page_height - 14.5, title)

    canvas.setFillColor(TEXT_SOFT)
    canvas.setFont(BODY_FONT_RAW, 7.8)
    page_label = f"{canvas.getPageNumber()}"
    canvas.drawRightString(page_width - doc.rightMargin, 12 * mm, page_label)
    canvas.drawString(doc.leftMargin, 12 * mm, "Threaded DB API Server")

    canvas.restoreState()


def build_pdf(source: Path, destination: Path) -> None:
    register_fonts()
    styles = make_styles()
    markdown_text = source.read_text(encoding="utf-8")
    html_text = md.markdown(
        markdown_text,
        extensions=["tables", "fenced_code", "sane_lists", "nl2br"],
    )

    doc = SimpleDocTemplate(
        str(destination),
        pagesize=PAGE_SIZE,
        leftMargin=LEFT_MARGIN,
        rightMargin=RIGHT_MARGIN,
        topMargin=TOP_MARGIN,
        bottomMargin=BOTTOM_MARGIN,
        title=source.stem,
        author="OpenAI Codex",
        pageCompression=1,
    )

    title, story = html_to_story(html_text, styles, doc.width, source.name)

    def on_first_page(canvas, current_doc):
        draw_page_frame(canvas, current_doc, title, True)

    def on_later_pages(canvas, current_doc):
        draw_page_frame(canvas, current_doc, title, False)

    doc.build(story, onFirstPage=on_first_page, onLaterPages=on_later_pages)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Render styled PDFs from Markdown files.")
    parser.add_argument("sources", nargs="+", help="Markdown files to render")
    parser.add_argument("--output-dir", default="output/pdf", help="Output directory for PDFs")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    for source_arg in args.sources:
        source = Path(source_arg)
        destination = output_dir / f"{source.stem}.pdf"
        build_pdf(source, destination)
        print(f"wrote {destination}")


if __name__ == "__main__":
    main()
