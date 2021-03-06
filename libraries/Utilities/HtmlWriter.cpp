#include <Arduino.h>
#include <HtmlWriter.h>
#include <math.h>

// Constructor
HtmlWriter::HtmlWriter(StringBuilder& output, const char* icon, const char* css, size_t maxBarLength)
    : _output(output), _icon(icon), _css(css), _maxBarLength(maxBarLength)
{
    _titlePrefix = "ESP";
    _bar = new char[maxBarLength + 1]; 
}


// Destructor
HtmlWriter::~HtmlWriter()
{
    delete[] _bar;
}


void HtmlWriter::setTitlePrefix(const char* prefix)
{
    _titlePrefix = prefix;
}


void HtmlWriter::writeHeader(String title, bool includeHomePageLink, bool includeHeading, uint16_t refreshInterval)
{
    _output.clear();
    _output.println(F("<html>"));
    
    _output.println(F("<head>"));
    _output.printf(F("<title>%s - %s</title>\r\n"), _titlePrefix, title.c_str());
    _output.printf(F("<link rel=\"stylesheet\" type=\"text/css\" href=\"%s\">\r\n"), _css);
    _output.printf(F("<link rel=\"icon\" sizes=\"128x128\" href=\"%s\">\r\n<link rel=\"apple-touch-icon-precomposed\" sizes=\"128x128\" href=\"%s\">\r\n"), _icon, _icon);
    if (refreshInterval > 0)
        _output.printf(F("<meta http-equiv=\"refresh\" content=\"%d\">\r\n") , refreshInterval);
    _output.println(F("</head>"));
    
    _output.println(F("<body>"));
    if (includeHomePageLink)
        _output.printf(F("<a href=\"/\"><img src=\"%s\"></a>"), _icon);
    if (includeHeading)
        _output.printf(F("<h1>%s</h1>\r\n"), title.c_str());
}


void HtmlWriter::writeFooter()
{
    _output.println(F("</body>"));
    _output.println(F("</html>"));
}


void HtmlWriter::writeBar(float value, String cssClass, bool fill, bool useDiv)
{
    value = std::max(std::min(value, 1.0f), 0.0f);
    size_t barLength = roundf(value * _maxBarLength);

    memset(_bar, 'o', barLength);
    _bar[barLength] = 0;

    if (useDiv) _output.print(F("<div>"));

    _output.printf(F("<span class=\"%s\">%s</span>"), cssClass.c_str(), _bar);

    if (fill)
    {
        memset(_bar, 'o', _maxBarLength - barLength);
        _bar[_maxBarLength - barLength] = 0;

        _output.printf(F("<span class=\"barFill\">%s</span>"), _bar);
    }
    else if (barLength == 0)
    {
        // Ensure that an empty bar has the same height
        _output.print(F("<span class=\"emptyBar\">o</span>"));
    }

    if (useDiv) _output.print("</div>");
}


void HtmlWriter::writeTextBox(String name, String label, String value,  uint16_t maxLength)
{
    _output.printf(
        F("<tr><td><label for=\"%s\">%s</label></td><td><input type=\"text\" name=\"%s\" value=\"%s\" maxlength=\"%d\"></td></tr>\r\n"), 
        name.c_str(),
        label.c_str(),
        name.c_str(),
        value.c_str(),
        maxLength
        );
}


void HtmlWriter::writeCheckbox(String name, String label, bool value)
{
    const char* checked = value ? "checked" : "";

    _output.printf(
        F("<tr><td><label for=\"%s\">%s</label></td><td><input type=\"checkbox\" name=\"%s\" value=\"true\" %s></td></tr>\r\n"), 
        name.c_str(),
        label.c_str(),
        name.c_str(),
        checked
        );
}


void HtmlWriter::writeRadioButtons(String name, String label, const char** values, int numValues, int index)
{
    _output.printf(
        F("<tr><td><label for=\"%s\">%s</label></td><td>"), 
        name.c_str(),
        label.c_str()
        );

    for (int i = 0; i < numValues; i++)
    {
        const char* checked = (i == index) ? "checked" : "";
        _output.printf(
            F("<input type=\"radio\" name=\"%s\" value=\"%d\" %s>%s"), 
            name.c_str(),
            i,
            checked,
            values[i]
            );
    }

    _output.println(F("</td></tr>"));
}


void HtmlWriter::writeSlider(String name, String label, String unitOfMeasure, int value, int minValue, int maxValue, int denominator)
{
    _output.printf(F("<tr><td><label for=\"%s\">%s</label></td><td>"), name.c_str(), label.c_str());
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
}
