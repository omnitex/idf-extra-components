import java.nio.charset.StandardCharsets
import java.nio.file.{Files, Paths, StandardOpenOption}

def jsonEscape(value: String): String =
  value
    .replace("\\", "\\\\")
    .replace("\"", "\\\"")
    .replace("\n", "\\n")
    .replace("\r", "\\r")
    .replace("\t", "\\t")

def jsonRecord(kind: String, file: String, line: Int, method: String, code: String): String =
  s"""{"kind":"${jsonEscape(kind)}","file":"${jsonEscape(file)}","line":$line,"method":"${jsonEscape(method)}","code":"${jsonEscape(code)}"}"""

@main def exportInventory(cpgFile: String, outFile: String) = {
  loadCpg(cpgFile)

  val methodRecords = cpg.method.internal.l.map { method =>
    val location = method.location
    jsonRecord(
      "method",
      location.filename,
      location.lineNumber.getOrElse(0),
      method.fullName,
      method.name
    )
  }

  val notableCallPattern =
    "(?i)(memcpy|memmove|memset|strcpy|strncpy|malloc|calloc|realloc|free|assert|" +
      "esp_(return|goto)_on_.*|.*lock.*|.*unlock.*|.*erase.*|.*write.*|.*read.*)"
  val callRecords = cpg.call.name(notableCallPattern).l.map { call =>
    val location = call.location
    jsonRecord(
      "call",
      location.filename,
      location.lineNumber.getOrElse(0),
      call.method.fullName,
      call.code
    )
  }

  Files.write(
    Paths.get(outFile),
    (methodRecords ++ callRecords).mkString("", "\n", "\n").getBytes(StandardCharsets.UTF_8),
    StandardOpenOption.CREATE,
    StandardOpenOption.TRUNCATE_EXISTING
  )
}
