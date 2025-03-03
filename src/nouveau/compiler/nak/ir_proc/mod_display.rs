use crate::args::RawArg;
use proc_macro2::{Span, TokenStream};
use syn::spanned::Spanned;
use syn::*;

#[derive(Default, Debug)]
pub struct ModifierDisplayArgs {
    pub name: Option<LitStr>,
    pub is_default: bool,
    pub prefix_name: bool,
}

impl syn::parse::Parse for ModifierDisplayArgs {
    fn parse(input: syn::parse::ParseStream) -> syn::Result<Self> {
        let mut args = ModifierDisplayArgs::default();

        if input.is_empty() {
            return Ok(args);
        }

        let unhandled_err =
            |span: Span| syn::Error::new(span, "Unhandled argument");

        for arg in
            syn::punctuated::Punctuated::<RawArg, Token![,]>::parse_terminated(
                input,
            )?
            .iter()
        {
            match arg {
                RawArg::Literal(name) => {
                    if args.name.is_none() {
                        args.name = Some(name.clone());
                    } else {
                        return Err(unhandled_err(name.span()));
                    }
                }
                RawArg::Ident(x) if x == "default" => {
                    if args.is_default {
                        return Err(unhandled_err(x.span()));
                    }
                    args.is_default = true;
                }
                RawArg::Ident(x) if x == "prefix_name" => {
                    if args.prefix_name {
                        return Err(unhandled_err(x.span()));
                    }
                    args.prefix_name = true;
                }
                x => return Err(unhandled_err(x.span())),
            }
        }

        Ok(args)
    }
}

struct ParsedField {
    ident: Ident,
    // Might be unused (e.g. Default or Wrapper without prefix_wrap is "")
    name: String,
    ty: FieldType,
}

enum FieldType {
    Simple,
    Wrapper { ty: Type, prefix_name: bool },
    Default,
}

fn variant_to_name(varname: &str) -> String {
    let mut res = String::new();
    let mut is_first = true;
    let mut last_is_num = false;
    let mut last_is_upper = false;

    // Convert CamelCase into snake_case
    // Special thingies: 1D, Rcp64H

    for c in varname.chars() {
        if c.is_uppercase() && !(is_first || last_is_num || last_is_upper) {
            res.push('_');
        }
        if is_first && c == '_' {
            // Workaround used to have ints in rust variants (_2D)
            continue;
        }
        is_first = false;
        last_is_num = c.is_numeric();
        last_is_upper = c.is_uppercase();

        res.extend(c.to_lowercase());
    }
    res
}

fn parse_variant(
    v: &Variant,
    attrib_name: &str,
    name_prefix: &str,
) -> Result<ParsedField> {
    let ident = v.ident.clone();

    let attr = v
        .attrs
        .iter()
        .filter(|x| x.path().is_ident(attrib_name))
        .next()
        .map(|attr| attr.parse_args::<ModifierDisplayArgs>())
        .transpose()?;

    let name = attr
        .as_ref()
        .and_then(|x| x.name.clone())
        .map(|x| x.value())
        .unwrap_or_else(|| variant_to_name(&ident.to_string()));
    let name = format!("{}{}", name_prefix, name);

    let field = match &v.fields {
        Fields::Unnamed(FieldsUnnamed { unnamed, .. })
            if unnamed.len() == 1 =>
        {
            // There must be one, see len check above
            let prefix_name = match &attr {
                Some(x) if x.is_default => {
                    return Err(syn::Error::new(
                        v.span(),
                        "Wrapper variant cannot have attributes",
                    ))
                }
                Some(x) => x.prefix_name,
                None => false,
            };
            FieldType::Wrapper {
                ty: unnamed.first().unwrap().ty.clone(),
                prefix_name,
            }
        }
        Fields::Unit => {
            if matches!(&attr, Some(x) if x.is_default) {
                FieldType::Default
            } else {
                FieldType::Simple
            }
        }
        _ => {
            return Err(syn::Error::new(
                v.span(),
                "Only supported unit fields and single wrapper fields",
            ))
        }
    };
    Ok(ParsedField {
        ident,
        name,
        ty: field,
    })
}

fn parse_fields(
    data: &DataEnum,
    attrib_name: &str,
    name_prefix: &str,
) -> syn::Result<Vec<ParsedField>> {
    let mut errors = Vec::new();
    let mut fields: Vec<_> = data
        .variants
        .iter()
        .map(|v| parse_variant(v, attrib_name, name_prefix))
        .filter_map(|r| r.map_err(|e| errors.push(e)).ok())
        .collect();

    if let Some(err) = errors.into_iter().reduce(|mut a, b| {
        a.combine(b);
        a
    }) {
        return Err(err);
    }

    // We use recursive-descent eager parser
    // if we have two modifiers that have common parts we must
    // parse them by longest first or we might be having correctness issues.
    // Ex: .cmp vs .cmp.exch
    // We need to order .cmp.exch BEFORE .cmp
    // The most stupid solution I got is to sort by string length
    // and hope we don't have this problem for sub-parsers
    fields.sort_by_key(|f| {
        match &f.ty {
            // hope we don't have names of 2**32 chars
            FieldType::Simple
            | FieldType::Wrapper {
                prefix_name: true, ..
            } => -(f.name.len() as i32),
            FieldType::Wrapper { .. } => 1, // Put wrapped almost last
            FieldType::Default => 0,        // Put default last
        }
    });
    Ok(fields)
}

fn emit_enum_display(
    enum_type: &Ident,
    fields: &[ParsedField],
) -> syn::Result<TokenStream> {
    let display_line = fields.iter().map(|f| {
        let id = &f.ident;
        let name = &f.name;
        match &f.ty {
            FieldType::Simple => quote! {
                #enum_type::#id => write!(f, #name),
            },
            FieldType::Wrapper { prefix_name, .. } => {
                let fmt_str =
                    format!("{}{{}}", if *prefix_name { name } else { "" });
                quote! {
                    #enum_type::#id(v) => write!(f, #fmt_str, v),
                }
            }
            FieldType::Default => quote! {
                #enum_type::#id => Ok(()),
            },
        }
    });

    Ok(quote! {
        impl ::std::fmt::Display for #enum_type {
            fn fmt(&self, f: &mut ::std::fmt::Formatter<'_>) -> ::std::fmt::Result {
                match self {
                    #(#display_line)*
                }
            }
        }
    })
}

fn emit_enum_parse(
    enum_type: &Ident,
    fields: &[ParsedField],
) -> syn::Result<TokenStream> {
    let parser = fields.iter()
        .filter_map(|field| {
            let id = &field.ident;
            let name = &field.name;
            match &field.ty {
                FieldType::Simple => Some(quote! {
                    crate::parser::tag(#name).map(|_| #enum_type::#id)
                }),
                FieldType::Wrapper{ ty, prefix_name } => {
                    let parse = quote! {
                        <#ty as crate::parser::WithDefaultParser>::parse.map(|x| #enum_type::#id(x))
                    };
                    let parse = if *prefix_name {
                        quote! {
                            crate::parser::tag(#name).and(#parse).map(|(_, x)| x)
                        }
                    } else { parse };

                    Some(parse)
                }
                // We cannot parse "" as default field
                // otherwise it would break our permutation parser
                FieldType::Default => None
            }
        });

    let err_str = format!("No variant of {enum_type} matches");
    Ok(quote! {
        impl crate::parser::WithDefaultParser for #enum_type {
            fn parse<'a>(input: &'a str) -> crate::parser::PResult<'a, Self> {
                #(
                    match #parser.parse(input) {
                        Ok((res, parsed)) => return Ok((res, parsed)),
                        Err(e) if e.is_unrecoverable => return Err(e),
                        _ => {},
                    }
                )*
                Err(crate::parser::ParseError::new(input, crate::parser::ErrorKind::Expected(#err_str)))
            }
        }
    })
}

pub fn derive_modifier(
    input: DeriveInput,
    display: bool,
    parse: bool,
) -> syn::Result<TokenStream> {
    let DeriveInput { ident, data, .. } = input;
    let enum_type = ident;

    let Data::Enum(data) = data else {
        return Err(syn::Error::new(
            Span::call_site(),
            "ModifierDisplay is only intended for enums",
        ));
    };

    let fields = parse_fields(&data, "modifier", ".")?;

    let num_default = fields
        .iter()
        .filter(|f| matches!(f.ty, FieldType::Default))
        .count();
    if num_default > 1 {
        return Err(syn::Error::new(
            Span::call_site(),
            "Can only have one default variant",
        ));
    }
    let mut tokens = TokenStream::new();
    if display {
        tokens.extend(emit_enum_display(&enum_type, &fields)?);
    }
    if parse {
        tokens.extend(emit_enum_parse(&enum_type, &fields)?);
    }
    Ok(tokens)
}

pub fn derive_enum(
    input: DeriveInput,
    display: bool,
    parse: bool,
) -> syn::Result<TokenStream> {
    let DeriveInput { ident, data, .. } = input;
    let enum_type = ident;

    let Data::Enum(data) = data else {
        return Err(syn::Error::new(
            Span::call_site(),
            "EnumDisplay is only intended for enums",
        ));
    };

    let fields = parse_fields(&data, "format", "")?;

    let has_default = fields.iter().any(|f| matches!(f.ty, FieldType::Default));
    if has_default {
        return Err(syn::Error::new(
            Span::call_site(),
            "No default variant possible",
        ));
    }

    let mut tokens = TokenStream::new();
    if display {
        tokens.extend(emit_enum_display(&enum_type, &fields)?);
    }
    if parse {
        tokens.extend(emit_enum_parse(&enum_type, &fields)?);
    }
    Ok(tokens)
}
