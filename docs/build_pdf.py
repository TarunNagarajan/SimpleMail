from pathlib import Path
import re, html
from reportlab.lib import colors
from reportlab.lib.enums import TA_LEFT
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import getSampleStyleSheet, ParagraphStyle
from reportlab.lib.units import mm
from reportlab.platypus import SimpleDocTemplate, Paragraph, Spacer, Preformatted, Table, TableStyle, PageBreak, Image

root = Path(__file__).resolve().parents[1]
source = root / 'docs' / 'SimpleMail-Technical-Guide.md'
target = root / 'docs' / 'SimpleMail, Documentation.pdf'

styles = getSampleStyleSheet()
styles.add(ParagraphStyle(name='GuideTitle', parent=styles['Title'], fontName='Helvetica-Bold', fontSize=23, leading=28, textColor=colors.black, spaceAfter=9))
styles.add(ParagraphStyle(name='GuideH2', parent=styles['Heading2'], fontName='Helvetica-Bold', fontSize=14, leading=17, textColor=colors.black, spaceBefore=10, spaceAfter=5, keepWithNext=True))
styles.add(ParagraphStyle(name='GuideH3', parent=styles['Heading3'], fontName='Helvetica-Bold', fontSize=11, leading=14, textColor=colors.black, spaceBefore=9, spaceAfter=4, keepWithNext=True))
styles.add(ParagraphStyle(name='GuideBody', parent=styles['BodyText'], fontName='Helvetica', fontSize=9.2, leading=12.3, spaceAfter=4, textColor=colors.black, alignment=TA_LEFT, allowWidows=0, allowOrphans=0))
styles.add(ParagraphStyle(name='GuideBullet', parent=styles['BodyText'], fontName='Helvetica', fontSize=9.2, leading=12.5, leftIndent=13, firstLineIndent=-8, spaceAfter=3))
styles.add(ParagraphStyle(name='GuideCode', fontName='Courier', fontSize=7.6, leading=9.4, leftIndent=7, rightIndent=7, spaceBefore=3, spaceAfter=7, textColor=colors.black))
styles.add(ParagraphStyle(name='GuideTable', parent=styles['BodyText'], fontName='Helvetica', fontSize=8, leading=10, spaceAfter=0))
styles.add(ParagraphStyle(name='GuideSmall', parent=styles['BodyText'], fontSize=8, leading=10, textColor=colors.black))

def inline(text):
    text = html.escape(text, quote=False)
    text = re.sub(r'`([^`]+)`', r'<font name="Courier">\1</font>', text)
    text = re.sub(r'\*\*([^*]+)\*\*', r'<b>\1</b>', text)
    text = re.sub(r'\[([^\]]+)\]\(([^)]+)\)', r'<u>\1</u>', text)
    return text

lines = source.read_text(encoding='utf-8').splitlines()
flow = []
image_count = 0
i = 0
while i < len(lines):
    raw = lines[i]
    s = raw.strip()
    if not s:
        i += 1; continue
    if s.startswith('!['):
        match = re.match(r'!\[([^]]+)\]\(([^)]+)\)', s)
        if match:
            if image_count > 1: flow.append(PageBreak())
            flow.append(Paragraph(inline(match[1]), styles['GuideH3']))
            im = Image(str(source.parent / match[2]))
            scale = min(174*mm/im.imageWidth, 218*mm/im.imageHeight)
            im.drawWidth=im.imageWidth*scale; im.drawHeight=im.imageHeight*scale
            flow.append(im); image_count += 1
        i += 1; continue
    if s.startswith('```'):
        i += 1; code = []
        while i < len(lines) and not lines[i].strip().startswith('```'):
            code.append(lines[i]); i += 1
        i += 1
        flow.append(Preformatted('\n'.join(code), styles['GuideCode'], maxLineLength=100))
        continue
    if s.startswith('# '):
        flow.append(Paragraph(inline(s[2:]), styles['GuideTitle'])); i += 1; continue
    if s == '## Ubuntu execution output': flow.append(PageBreak())
    if s.startswith('## '):
        flow.append(Paragraph(inline(s[3:]), styles['GuideH2'])); i += 1; continue
    if s.startswith('### '):
        flow.append(Paragraph(inline(s[4:]), styles['GuideH3'])); i += 1; continue
    if s.startswith('|'):
        rows = []
        while i < len(lines) and lines[i].strip().startswith('|'):
            row = [cell.strip() for cell in lines[i].strip().strip('|').split('|')]
            if not all(re.fullmatch(r':?-{3,}:?', c) for c in row):
                rows.append([Paragraph(inline(c), styles['GuideTable']) for c in row])
            i += 1
        if rows:
            cols = max(map(len, rows)); widths = [32*mm, 142*mm] if cols == 2 else [45*mm]*cols
            rows = [r + [Paragraph('', styles['GuideTable'])]*(cols-len(r)) for r in rows]
            t = Table(rows, colWidths=widths[:cols], repeatRows=1, hAlign='LEFT')
            t.setStyle(TableStyle([
                ('BACKGROUND',(0,0),(-1,0),colors.white),
                ('TEXTCOLOR',(0,0),(-1,0),colors.black),
                ('LINEBELOW',(0,0),(-1,0),0.8,colors.black),
                ('LINEBELOW',(0,1),(-1,-1),0.25,colors.black),
                ('VALIGN',(0,0),(-1,-1),'TOP'),('LEFTPADDING',(0,0),(-1,-1),6),
                ('RIGHTPADDING',(0,0),(-1,-1),6),('TOPPADDING',(0,0),(-1,-1),5),
                ('BOTTOMPADDING',(0,0),(-1,-1),5),
            ]))
            flow.extend([Spacer(1,3),t,Spacer(1,7)])
        continue
    if s.startswith('- ') or s.startswith('* '):
        flow.append(Paragraph('• ' + inline(s[2:]), styles['GuideBullet'])); i += 1; continue
    para = [s]; i += 1
    while i < len(lines) and lines[i].strip() and not lines[i].strip().startswith(('#','```','|','- ','* ')):
        para.append(lines[i].strip()); i += 1
    flow.append(Paragraph(inline(' '.join(para)), styles['GuideBody']))

def page(canvas, doc):
    canvas.saveState()
    w,h = A4
    canvas.setStrokeColor(colors.black)

    canvas.setFont('Helvetica', 8)
    canvas.setFillColor(colors.black)

    canvas.drawRightString(w-18*mm, 10*mm, f'{doc.page}')
    canvas.restoreState()

doc = SimpleDocTemplate(str(target), pagesize=A4, rightMargin=18*mm, leftMargin=18*mm, topMargin=17*mm, bottomMargin=18*mm, title='SimpleMail Technical Guide', author='SimpleMail Project Group')
doc.build(flow, onFirstPage=page, onLaterPages=page)
print(target)
