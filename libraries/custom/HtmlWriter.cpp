#include <Arduino.h>
#include <HtmlWriter.h>
#include <math.h>

// Constructor
HtmlWriter::HtmlWriter(StringBuilder& output, PGM_P icon, PGM_P css, size_t maxBarLength)
    : _output(output), _icon(FPSTR(icon)), _css(FPSTR(css)), _maxBarLength(maxBarLength)
{
    _titlePrefix = F("ESP");
    _bar = new char[maxBarLength + 1]; 
}


// Destructor
HtmlWriter::~HtmlWriter()
{
    delete[] _bar;
}


void HtmlWriter::setTitlePrefix(const String& prefix)
{
    _titlePrefix = prefix;
}


void HtmlWriter::writeHeader(const String& title, bool includeHomePageLink, bool includeHeading, uint16_t refreshInterval)
{
    _output.clear();
    _output.println(F("<!DOCTYPE html>"));
    _output.println(F("<html lang=\"en-US\">"));
    
    _output.println(F("<head>"));
    _output.printf(F("<title>%s - %s</title>\r\n"), _titlePrefix.c_str(), title.c_str());
    _output.printf(F("<link rel=\"stylesheet\" type=\"text/css\" href=\"%s\">\r\n"), _css.c_str());
    _output.printf(F("<link rel=\"icon\" sizes=\"128x128\" href=\"%s\">\r\n"), _icon.c_str());
    _output.printf(F("<link rel=\"apple-touch-icon-precomposed\" sizes=\"128x128\" href=\"%s\">\r\n"), _icon.c_str());
    _output.println(F("<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">"));
    if (refreshInterval > 0)
        _output.printf(F("<meta http-equiv=\"refresh\" content=\"%d\">\r\n") , refreshInterval);
    _output.println(F("</head>"));
    
    _output.println(F("<body>"));
    if (includeHomePageLink)
        _output.printf(F("<a href=\"/\"><img src=\"%s\"></a>"), _icon.c_str());
    if (includeHeading)
        _output.printf(F("<h1>%s</h1>\r\n"), title.c_str());
}


void HtmlWriter::writeHeader(const String& title, const Navigation& navigation, uint16_t refreshInterval)
{
    writeHeader(title, false, false, refreshInterval);

    _output.println(F("<script>"));
    _output.println(F("function setNavWidth(w) { document.getElementById(\"nav\").style.width = w; }"));
    _output.println(F("</script>"));

    _output.println(F("<header>"));

    const char* heading = (title == F("Home")) ? _titlePrefix.c_str() : title.c_str();

    _output.printf(
        F("<a href=\"javascript:setNavWidth('%s')\" class=\"openbtn\">&#9776;</a>%s<a href=\"/\" class=\"logo\"></a>\r\n"),
        navigation.width.c_str(),
        heading
        );

    _output.println(F("</header>"));
    _output.println(F("<nav id=\"nav\">"));
    _output.println(F("<a href=\"javascript:setNavWidth('0')\" class=\"closebtn\">&times;</a>"));
    for (const MenuItem& menuItem : navigation.menuItems)
    {
        String urlPath = F("/");
        if (menuItem.urlPath != nullptr)
            urlPath += FPSTR(menuItem.urlPath);
        _output.printf(F("<a href=\"%s\">"), urlPath.c_str());
        if (menuItem.icon != nullptr)
        {
            char icon[32];
            strncpy_P(icon, menuItem.icon, sizeof(icon));
            _output.printf(F("<img class=\"icon\" src=\"%s\">"), icon);
        }
        _output.print(FPSTR(menuItem.label));
        _output.println(F("</a>"));
    }
    _output.println(F("</nav>"));
}


void HtmlWriter::writeFooter()
{
    _output.println(F("</body>"));
    _output.println(F("</html>"));
}


void HtmlWriter::writeBar(float value, const String& cssClass, bool fill, bool useDiv, size_t maxBarLength)
{
    char* bar;
    if (maxBarLength == 0)
    {
        bar = _bar;
        maxBarLength = _maxBarLength;
    }
    else
        bar = new char[maxBarLength + 1];

    value = std::max(std::min(value, 1.0F), 0.0F);
    size_t barLength = roundf(value * maxBarLength);

    memset(bar, 'o', barLength);
    bar[barLength] = 0;

    if (useDiv) _output.print(F("<div>"));

    _output.printf(F("<span class=\"%s\">%s</span>"), cssClass.c_str(), bar);

    if (fill)
    {
        memset(bar, 'o', maxBarLength - barLength);
        bar[maxBarLength - barLength] = 0;

        _output.printf(F("<span class=\"barFill\">%s</span>"), bar);
    }
    else if (barLength == 0)
    {
        // Ensure that an empty bar has the same height
        _output.print(F("<span class=\"emptyBar\">o</span>"));
    }

    if (useDiv) _output.print("</div>");

    if (bar != _bar)
    {
        delete[] bar;
    }
}


void HtmlWriter::writeStackedBar(float value1, float value2, const String& cssClass1, const String& cssClass2, bool fill, bool useDiv)
{
    value1 = std::max(std::min(value1, 1.0f), 0.0f);
    value2 = std::max(std::min(value2, 1.0f - value1), 0.0f);
    size_t barLength1 = roundf(value1 * _maxBarLength);
    size_t barLength2 = roundf(value2 * _maxBarLength);

    if (useDiv) _output.print(F("<div>"));

    memset(_bar, 'o', barLength1);
    _bar[barLength1] = 0;

    _output.printf(F("<span class=\"%s\">%s</span>"), cssClass1.c_str(), _bar);

    memset(_bar, 'o', barLength2);
    _bar[barLength2] = 0;

    _output.printf(F("<span class=\"%s\">%s</span>"), cssClass2.c_str(), _bar);

    if (fill)
    {
        memset(_bar, 'o', _maxBarLength - barLength1 - barLength2);
        _bar[_maxBarLength - barLength1 - barLength2] = 0;

        _output.printf(F("<span class=\"barFill\">%s</span>"), _bar);
    }
    else if (barLength1 == 0 && barLength2 == 0)
    {
        // Ensure that an empty bar has the same height
        _output.print(F("<span class=\"emptyBar\">o</span>"));
    }

    if (useDiv) _output.print("</div>");
}


void HtmlWriter::writeGraphCell(float value, const String& barCssClass, bool fill, size_t maxBarLength)
{
    writeCellStart(F("graph"));
    writeBar(value, barCssClass, fill, false, maxBarLength);
    writeCellEnd();
}


void HtmlWriter::writeGraphCell(float value1, float value2, const String& barCssClass1, const String& barCssClass2, bool fill)
{
    writeCellStart(F("graph"));
    writeStackedBar(value1, value2, barCssClass1, barCssClass2, fill, false);
    writeCellEnd();
}


void HtmlWriter::writeFormStart(const String& action, const String& cssClass)
{
    _output.printf(
        F("<form action=\"%s\" method=\"POST\" class=\"%s\">\r\n"),
        action.c_str(),
        cssClass.c_str());
}


void HtmlWriter::writeFormEnd()
{
    _output.println(F("</form>"));
}


void HtmlWriter::writeSubmitButton()
{
    _output.println(F("<input type=\"submit\">"));
}


void HtmlWriter::writeSubmitButton(const String& label, const String& cssClass)
{
    _output.printf(
        F("<input type=\"submit\" value=\"%s\" class=\"%s\">\r\n"),
        label.c_str(),
        cssClass.c_str());
}


void HtmlWriter::writeLabel(const String& label, const String& forId)
{
    _output.printf(
        F("<label for=\"%s\">%s</label>"), 
        forId.c_str(),
        label.c_str());
}


void HtmlWriter::writeTextBox(
    const String& name,
    const String& label,
    const String& value,
    uint16_t maxLength,
    const String& type)
{
    writeLabel(label, name);
    _output.printf(
        F("<input type=\"%s\" id=\"%s\" name=\"%s\" value=\"%s\" maxlength=\"%d\">\r\n"), 
        type.c_str(),
        name.c_str(),
        name.c_str(),
        value.c_str(),
        maxLength);
}


void HtmlWriter::writeNumberBox(
    const String& name,
    const String& label,
    float value,
    float minValue,
    float maxValue,
    int decimals)
{
    float step = pow10f(-decimals);

    static char format[128];
    snprintf(
        format,
        sizeof(format),
        "<input type=\"number\" id=\"%%s\" name=\"%%s\" value=\"%%0.%df\" min=\"%%0.%df\" max=\"%%0.%df\" step=\"%%0.%df\">\r\n",
        decimals, decimals, decimals, decimals);

    writeLabel(label, name);
    _output.printf(
        FPSTR(format), 
        name.c_str(),
        name.c_str(),
        value,
        minValue,
        maxValue,
        step);
}


void HtmlWriter::writeCheckbox(const String& name, const String& label, bool value)
{
    const char* checked = value ? "checked" : "";

    writeLabel(label, name);
    _output.printf(
        F("<input type=\"checkbox\" id=\"%s\" name=\"%s\" value=\"true\" %s>\r\n"), 
        name.c_str(),
        name.c_str(),
        checked);
}


void HtmlWriter::writeRadioButtons(const String& name, const String& label, const char** values, int numValues, int index)
{
    writeLabel(label, name);
    writeDivStart();

    for (int i = 0; i < numValues; i++)
    {
        const char* checked = (i == index) ? "checked" : "";

        writeDivStart();
        _output.printf(
            F("<input type=\"radio\" id=\"%s%d\" name=\"%s\" value=\"%d\" %s><label for=\"%s%d\">%s</label>"), 
            name.c_str(),
            i,
            name.c_str(),
            i,
            checked,
            name.c_str(),
            i,
            values[i]);
        writeDivEnd();
    }

    writeDivEnd();
}


void HtmlWriter::writeSlider(const String& name, const String& label, const String& unitOfMeasure, int value, int minValue, int maxValue, int denominator)
{
    writeLabel(label, name);
    writeDivStart();

    _output.printf(
        F("<div><input name=\"%s\" type=\"range\" min=\"%d\" max=\"%d\" value=\"%d\"></div>"),
        name.c_str(),
        minValue,
        maxValue,
        value
        );
    
    if (denominator == 1)
        _output.printf(F("<div>%d %s</div></td></tr>\r\n"), value, unitOfMeasure.c_str());
    else
        _output.printf(F("<div>%0.3f %s</div></td></tr>\r\n"), float(value) / denominator, unitOfMeasure.c_str());

    writeDivEnd();
}


void HtmlWriter::writeHeading(const String& title, int level)
{
    _output.printf(
        F("<h%d>%s</h%d>\r\n"),
        level,
        title.c_str(),
        level);
}


void HtmlWriter::writeSectionStart(const String& title)
{
    _output.println(F("<section>"));
    writeHeading(title, 1);
}


void HtmlWriter::writeSectionEnd()
{
    _output.println(F("</section>"));
}


void HtmlWriter::writeDivStart(const String& cssClass)
{
    if (cssClass.length() == 0)
        _output.println(F("<div>"));
    else
        _output.printf(F("<div class=\"%s\">\r\n"), cssClass.c_str());
}


void HtmlWriter::writeDivEnd()
{
    _output.println(F("</div>"));
}


void HtmlWriter::writeDiv(const String& content, const String& cssClass)
{
    writeDivStart(cssClass);
    _output.print(content);
    writeDivEnd();
}


void HtmlWriter::writeTableStart()
{
    _output.println(F("<table>"));
}


void HtmlWriter::writeTableEnd()
{
    _output.println(F("</table>"));
}


void HtmlWriter::writeRowStart()
{
    _output.print(F("<tr>"));
}


void HtmlWriter::writeRowEnd()
{
    _output.println(F("</tr>"));
}


void HtmlWriter::writeCellStart(const String& cssClass)
{
    _output.printf(
        F("<td class=\"%s\">"),
        cssClass.c_str());
}


void HtmlWriter::writeCellEnd()
{
    _output.print(F("</td>"));
}


void HtmlWriter::writeHeaderCell(const String& value, int colspan, int rowspan)
{
    _output.print(F("<th"));
    if (colspan > 0) _output.printf(F(" colspan=\"%d\""), colspan);
    if (rowspan > 0) _output.printf(F(" rowspan=\"%d\""), rowspan);
    _output.print(F(">"));
    _output.print(value);
    _output.print(F("</th>"));
}


void HtmlWriter::writeCell(const String& value)
{
    return writeCell(value.c_str());
}


void HtmlWriter::writeCell(const char* value)
{
    _output.print(F("<td>"));
    _output.print(value);
    _output.print(F("</td>"));
}


void HtmlWriter::writeCell(int value)
{
    _output.print(F("<td>"));
    _output.printf(F("%d"), value);
    _output.print(F("</td>"));
}

void HtmlWriter::writeCell(uint32_t value)
{
    _output.print(F("<td>"));
    _output.printf(F("%u"), value);
    _output.print(F("</td>"));
}


void HtmlWriter::writeCell(float value, const __FlashStringHelper* format)
{
    if (format == nullptr)
        format = F("%0.1f");

    _output.print(F("<td>"));
    _output.printf(format, value);
    _output.print(F("</td>"));
}


void HtmlWriter::writeRow(const String& name, const String& format, ...)
{
    static char value[64];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(value, sizeof(value) - 1, format.c_str(), args);
    value[length] = 0;
    va_end(args);

    _output.printf(
        F("<tr><th>%s</th><td>%s</td></tr>\r\n"),
        name.c_str(),
        value);
}


void HtmlWriter::writePager(int totalPages, int currentPage)
{
    writeDivStart(F("pager"));
    for (int i = 0; i < totalPages; i++)
    {
        if (i == currentPage)
            _output.printf(F("<span>%d</span>"), i + 1);
        else
            _output.printf(F("<a href='?page=%d'>%d</a>"), i, i + 1);           
    }
    writeDivEnd();
}


void HtmlWriter::writeParagraph(const String& innerHtml)
{
    _output.print(F("<p>"));
    _output.print(innerHtml);
    _output.println(F("</p>"));
}


void HtmlWriter::writeActionLink(
    const String& action,
    const String& label,
    time_t currentTime,
    const String& cssClass,
    const String& icon)
{
    _output.printf(
        F("<a class=\"%s\" href=\"?%s=%u\">"),
        cssClass.c_str(),
        action.c_str(),
        currentTime);
    if (icon.length() != 0)
        _output.printf(F("<img class=\"icon\" src=\"%s\">"), icon.c_str());
    _output.print(label);
    _output.print(F("</a>"));
}